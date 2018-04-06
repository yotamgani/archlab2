#include <stdlib.h>
#include <stdio.h>
#include <string.h>
//#include <unistd.h>
#include <sys/types.h> 
//#include <sys/socket.h>
//#include <netinet/in.h>

#include "llsim.h"

#define sp_printf(a...)						\
	do {							\
		llsim_printf("sp: clock %d: ", llsim->clock);	\
		llsim_printf(a);				\
	} while (0)

int nr_simulated_instructions = 0;
FILE *inst_trace_fp = NULL, *cycle_trace_fp = NULL;

typedef struct sp_registers_s {
	// 6 32 bit registers (r[0], r[1] don't exist)
	int r[8];

	// 16 bit program counter
	int pc;

	// 32 bit instruction
	int inst;

	// 5 bit opcode
	int opcode;

	// 3 bit destination register index
	int dst;

	// 3 bit source #0 register index
	int src0;

	// 3 bit source #1 register index
	int src1;

	// 32 bit alu #0 operand
	int alu0;

	// 32 bit alu #1 operand
	int alu1;

	// 32 bit alu output
	int aluout;

	// 32 bit immediate field (original 16 bit sign extended)
	int immediate;

	// 32 bit cycle counter
	int cycle_counter;

	// 3 bit control state machine state register
	int ctl_state;

	//2 bit DMA control state machine 
	int DMA_state;

	//32 bit DMA source memory address
	int DMA_src;

	//32 bit DMA destination memory address
	int DMA_dst;

	//32 bit DMA data
	int DMA_data;

	//32 bit DMA count
	unsigned int DMA_count;

	// control states
	#define CTL_STATE_IDLE		0
	#define CTL_STATE_FETCH0	1
	#define CTL_STATE_FETCH1	2
	#define CTL_STATE_DEC0		3
	#define CTL_STATE_DEC1		4
	#define CTL_STATE_EXEC0		5
	#define CTL_STATE_EXEC1		6

	// DMA states
	#define DMA_STATE_IDLE		 0
	#define DMA_STATE_MEM_READ	 1
	#define DMA_STATE_MEM_SAMPLE 2
	#define DMA_STATE_MEM_WRITE	 3

} sp_registers_t;

/*
 * Master structure
 */
typedef struct sp_s {
	// local sram
#define SP_SRAM_HEIGHT	64 * 1024
	llsim_memory_t *sram;

	unsigned int memory_image[SP_SRAM_HEIGHT];
	int memory_image_size;

	sp_registers_t *spro, *sprn;
	
	int start;
} sp_t;

static void sp_reset(sp_t *sp)
{
	sp_registers_t *sprn = sp->sprn;

	memset(sprn, 0, sizeof(*sprn));
}

/*
 * opcodes
 */
#define ADD 0
#define SUB 1
#define LSF 2
#define RSF 3
#define AND 4
#define OR  5
#define XOR 6
#define LHI 7
#define LD 8
#define ST 9
#define JLT 16
#define JLE 17
#define JEQ 18
#define JNE 19
#define JIN 20
 //DMA opcode
#define MEMCPY 21
#define DMAPOL 22

#define HLT 24



static char opcode_name[32][4] = {"ADD", "SUB", "LSF", "RSF", "AND", "OR", "XOR", "LHI",
				 "LD", "ST", "U", "U", "U", "U", "U", "U",
				 "JLT", "JLE", "JEQ", "JNE", "JIN", "MEMCPY", "DMAPOL", "U",
				 "HLT", "U", "U", "U", "U", "U", "U", "U"};

static void dump_sram(sp_t *sp)
{
	FILE *fp;
	int i;

	fp = fopen("sram_out.txt", "w");
	if (fp == NULL) {
                printf("couldn't open file sram_out.txt\n");
                exit(1);
	}
	for (i = 0; i < SP_SRAM_HEIGHT; i++)
		fprintf(fp, "%08x\n", llsim_mem_extract(sp->sram, i, 31, 0));
	fclose(fp);
}


static void sp_ctl(sp_t *sp)
{
	sp_registers_t *spro = sp->spro;
	sp_registers_t *sprn = sp->sprn;
	int i;

	// sp_ctl

	fprintf(cycle_trace_fp, "cycle %d\n", spro->cycle_counter);
	for (i = 2; i <= 7; i++)
		fprintf(cycle_trace_fp, "r%d %08x\n", i, spro->r[i]);
	fprintf(cycle_trace_fp, "pc %08x\n", spro->pc);
	fprintf(cycle_trace_fp, "inst %08x\n", spro->inst);
	fprintf(cycle_trace_fp, "opcode %08x\n", spro->opcode);
	fprintf(cycle_trace_fp, "dst %08x\n", spro->dst);
	fprintf(cycle_trace_fp, "src0 %08x\n", spro->src0);
	fprintf(cycle_trace_fp, "src1 %08x\n", spro->src1);
	fprintf(cycle_trace_fp, "immediate %08x\n", spro->immediate);
	fprintf(cycle_trace_fp, "alu0 %08x\n", spro->alu0);
	fprintf(cycle_trace_fp, "alu1 %08x\n", spro->alu1);
	fprintf(cycle_trace_fp, "aluout %08x\n", spro->aluout);
	fprintf(cycle_trace_fp, "cycle_counter %08x\n", spro->cycle_counter);
	fprintf(cycle_trace_fp, "ctl_state %08x\n\n", spro->ctl_state);
	fprintf(cycle_trace_fp, "DMA_state %08x\n\n", spro->DMA_state);
	fprintf(cycle_trace_fp, "DMA_count %08x\n\n", spro->DMA_count);
	fprintf(cycle_trace_fp, "DMA_src %08x\n\n", spro->DMA_src);
	fprintf(cycle_trace_fp, "DMA_dst %08x\n\n", spro->DMA_dst);
	fprintf(cycle_trace_fp, "DMA_data %08x\n\n", spro->DMA_data);


	sprn->cycle_counter = spro->cycle_counter + 1;

	switch (spro->ctl_state) {
	case CTL_STATE_IDLE:
		sprn->pc = 0;
		if (sp->start)
		{
			sprn->ctl_state = CTL_STATE_FETCH0;
			sp->start = 0;
		}
		else
		{
			dump_sram(sp);
			llsim_stop();
		}
		break;

	case CTL_STATE_FETCH0:
		llsim_mem_read(sp->sram, sp->spro->pc);
		sprn->ctl_state = CTL_STATE_FETCH1;
		break;

	case CTL_STATE_FETCH1:
		sprn->inst = llsim_mem_extract_dataout(sp->sram, 31, 0);
		sprn->ctl_state = CTL_STATE_DEC0;
		break;

	case CTL_STATE_DEC0:
		sprn->opcode = (unsigned int)generic_extract_bits(&spro->inst, 29, 25);
		sprn->dst = (unsigned int)generic_extract_bits(&spro->inst, 24, 22);
		sprn->src0 = (unsigned int)generic_extract_bits(&spro->inst, 21, 19);
		sprn->src1 = (unsigned int)generic_extract_bits(&spro->inst, 18, 16);
		sprn->immediate = generic_extract_bits(&spro->inst, 15, 0);
		sprn->ctl_state = CTL_STATE_DEC1;
		break;

	case CTL_STATE_DEC1:
		sprn->r[1] = spro->immediate;
		switch (sprn->src0)
		{
		case 0:
			sprn->alu0 = 0;
			break;
		case 1:
			sprn->alu0 = spro->immediate;
			break;
		default:
			sprn->alu0 = sp->spro->r[spro->src0];
		}
		switch (sprn->src1)
		{
		case 0:
			sprn->alu1 = 0;
			break;
		case 1:
			sprn->alu1= spro->immediate;
			break;
		default:
			sprn->alu1 = sp->spro->r[spro->src1];
		}
		sprn->ctl_state = CTL_STATE_EXEC0;
		break;


	case CTL_STATE_EXEC0:
		switch (spro->opcode)
		{
		case ADD:
			sprn->aluout = spro->alu0 + spro->alu1;
			break;
		case SUB:
			sprn->aluout = spro->alu0 - spro->alu1;
			break;
		case LSF:
			sprn->aluout = spro->alu0 << spro->alu1;
			break;
		case RSF:
			sprn->aluout = spro->alu0 >> spro->alu1;
			break;
		case AND:
			sprn->aluout = spro->alu0 & spro->alu1;
			break;
		case OR:
			sprn->aluout = spro->alu0 | spro->alu1;
			break;
		case XOR:
			sprn->aluout = spro->alu0 ^ spro->alu1;
			break;
		case LHI:
			sprn->aluout = (spro->alu1 << 16) | generic_extract_bits(&spro->alu0, 15, 0);
			break;
		case JLT:
			sprn->aluout = (spro->alu0 < spro->alu1) ? 1 : 0;
			break;
		case JLE:
			sprn->aluout = (spro->alu0 <= spro->alu1) ? 1 : 0;
			break;
		case JEQ:
			sprn->aluout = (spro->alu0 == spro->alu1) ? 1 : 0;
			break;
		case JNE:
			sprn->aluout = (spro->alu0 != spro->alu1) ? 1 : 0;
			break;
		case LD:
			llsim_mem_read(sp->sram, spro->alu1);
			break;
		case MEMCPY:
			if (spro->DMA_state != DMA_STATE_IDLE)
				break; //DMA is busy, do nothing
			sprn->DMA_state = DMA_STATE_MEM_READ;
			sprn->DMA_count = spro->immediate;
			sprn->DMA_src = spro->alu0;
			sprn->DMA_dst = spro->alu1;
			break;
		case DMAPOL:
			sprn->aluout = (spro->DMA_state == DMA_STATE_IDLE) ? 1 : 0; //0 -> DMA is busy, 1 -> DMA is available
			break;
		default:
			break; //do nothing
		}
		sprn->ctl_state = CTL_STATE_EXEC1;
		break;

	case CTL_STATE_EXEC1:
		sprn->pc = spro->pc + 1 % 0xffff; //Increase PC
		sprn->ctl_state = CTL_STATE_FETCH0;
		nr_simulated_instructions++;
		switch (sp->spro->opcode)
		{
		case LD:
			sprn->r[spro->dst] = llsim_mem_extract_dataout(sp->sram, 31, 0);
			break;
		case ST:
			llsim_mem_set_datain(sp->sram, spro->r[spro->src0], 31, 0);
			llsim_mem_write(sp->sram, spro->r[spro->src1]);
			break;
		case JLT:
		case JLE:
		case JEQ:
		case JNE:
			if (spro->aluout == 1) //jump taken
			{
				sprn->pc = spro->immediate;
				sprn->r[7] = spro->pc;
			}
			break;
		case JIN:
			sprn->pc = spro->src0;
			sprn->r[7] = spro->pc;
			break;
		case HLT:
			sprn->ctl_state = CTL_STATE_IDLE;
			break;
		case MEMCPY:
			break;
		default: //all other opcodes
			sprn->r[spro->dst] = spro->aluout;
		}
		break;
	}

	if (((spro->ctl_state == CTL_STATE_FETCH0) || (spro->ctl_state == CTL_STATE_EXEC0 && spro->opcode == LD) ||
		(spro->ctl_state == CTL_STATE_EXEC1 && spro->opcode == ST)) && spro->DMA_state != DMA_STATE_MEM_SAMPLE) 
		return; //memory is busy, and DMA is not in sample state (that does not occupy memory) -> DMA does nothing.

	//else: memory is free to use by DMA
	switch (spro->DMA_state)
	{
	case DMA_STATE_IDLE:
		break; //wait for control to switch to READ state
	case DMA_STATE_MEM_READ:
		if (spro->DMA_count > 0)
		{
			llsim_mem_read(sp->sram, spro->DMA_src);
			sprn->DMA_state = DMA_STATE_MEM_SAMPLE;
		}
		else
			sprn->DMA_state = DMA_STATE_IDLE;
		break;
	case DMA_STATE_MEM_SAMPLE:
		sprn->DMA_data = llsim_mem_extract_dataout(sp->sram, 31, 0);
		sprn->DMA_state = DMA_STATE_MEM_WRITE;
		break;
	case DMA_STATE_MEM_WRITE:
		llsim_mem_set_datain(sp->sram, spro->DMA_data, 31, 0);
		llsim_mem_write(sp->sram, spro->DMA_dst);
		sprn->DMA_count = spro->DMA_count - 1;
		sprn->DMA_src = spro->DMA_src + 1;
		sprn->DMA_dst = spro->DMA_dst + 1;
		if (spro->DMA_count - 1 == 0)
			sprn->DMA_state = DMA_STATE_IDLE;
		else
			sprn->DMA_state = DMA_STATE_MEM_READ;
		break;
	default:
		break;
	}
}

static void sp_run(llsim_unit_t *unit)
{
	sp_t *sp = (sp_t *) unit->private;

	if (llsim->reset) {
		sp_reset(sp);
		return;
	}

	sp->sram->read = 0;
	sp->sram->write = 0;

	sp_ctl(sp);
}

static void sp_generate_sram_memory_image(sp_t *sp, char *program_name)
{
        FILE *fp;
        int addr, i;

        fp = fopen(program_name, "r");
        if (fp == NULL) {
                printf("couldn't open file %s\n", program_name);
                exit(1);
        }
        addr = 0;
        while (addr < SP_SRAM_HEIGHT) {
                fscanf(fp, "%08x\n", &sp->memory_image[addr]);
                addr++;
                if (feof(fp))
                        break;
        }
	sp->memory_image_size = addr;

        fprintf(inst_trace_fp, "program %s loaded, %d lines\n", program_name, addr);

	for (i = 0; i < sp->memory_image_size; i++)
		llsim_mem_inject(sp->sram, i, sp->memory_image[i], 31, 0);
}

static void sp_register_all_registers(sp_t *sp)
{
	sp_registers_t *spro = sp->spro, *sprn = sp->sprn;

	// registers
	llsim_register_register("sp", "r_0", 32, 0, &spro->r[0], &sprn->r[0]);
	llsim_register_register("sp", "r_1", 32, 0, &spro->r[1], &sprn->r[1]);
	llsim_register_register("sp", "r_2", 32, 0, &spro->r[2], &sprn->r[2]);
	llsim_register_register("sp", "r_3", 32, 0, &spro->r[3], &sprn->r[3]);
	llsim_register_register("sp", "r_4", 32, 0, &spro->r[4], &sprn->r[4]);
	llsim_register_register("sp", "r_5", 32, 0, &spro->r[5], &sprn->r[5]);
	llsim_register_register("sp", "r_6", 32, 0, &spro->r[6], &sprn->r[6]);
	llsim_register_register("sp", "r_7", 32, 0, &spro->r[7], &sprn->r[7]);

	llsim_register_register("sp", "pc", 16, 0, &spro->pc, &sprn->pc);
	llsim_register_register("sp", "inst", 32, 0, &spro->inst, &sprn->inst);
	llsim_register_register("sp", "opcode", 5, 0, &spro->opcode, &sprn->opcode);
	llsim_register_register("sp", "dst", 3, 0, &spro->dst, &sprn->dst);
	llsim_register_register("sp", "src0", 3, 0, &spro->src0, &sprn->src0);
	llsim_register_register("sp", "src1", 3, 0, &spro->src1, &sprn->src1);
	llsim_register_register("sp", "alu0", 32, 0, &spro->alu0, &sprn->alu0);
	llsim_register_register("sp", "alu1", 32, 0, &spro->alu1, &sprn->alu1);
	llsim_register_register("sp", "aluout", 32, 0, &spro->aluout, &sprn->aluout);
	llsim_register_register("sp", "immediate", 32, 0, &spro->immediate, &sprn->immediate);
	llsim_register_register("sp", "cycle_counter", 32, 0, &spro->cycle_counter, &sprn->cycle_counter);
	llsim_register_register("sp", "ctl_state", 3, 0, &spro->ctl_state, &sprn->ctl_state);
}

void sp_init(char *program_name)
{
	llsim_unit_t *llsim_sp_unit;
	llsim_unit_registers_t *llsim_ur;
	sp_t *sp;

	llsim_printf("initializing sp unit\n");

	inst_trace_fp = fopen("inst_trace.txt", "w");
	if (inst_trace_fp == NULL) {
		printf("couldn't open file inst_trace.txt\n");
		exit(1);
	}

	cycle_trace_fp = fopen("cycle_trace.txt", "w");
	if (cycle_trace_fp == NULL) {
		printf("couldn't open file cycle_trace.txt\n");
		exit(1);
	}

	llsim_sp_unit = llsim_register_unit("sp", sp_run);
	llsim_ur = llsim_allocate_registers(llsim_sp_unit, "sp_registers", sizeof(sp_registers_t));
	sp = llsim_malloc(sizeof(sp_t));
	llsim_sp_unit->private = sp;
	sp->spro = llsim_ur->old;
	sp->sprn = llsim_ur->new;

	sp->sram = llsim_allocate_memory(llsim_sp_unit, "sram", 32, SP_SRAM_HEIGHT, 0);
	sp_generate_sram_memory_image(sp, program_name);

	sp->start = 1;

	sp_register_all_registers(sp);
}

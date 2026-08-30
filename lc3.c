#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>

enum {
    MR_KBSR = 0xFE00,
    MR_KBDR = 0xFE02
};

// Trap Codes
enum {
    TRAP_GETC = 0x20,
    TRAP_OUT = 0x21,
    TRAP_PUTS = 0x22,
    TRAP_IN = 0x23,
    TRAP_PUTSP = 0x24,
    TRAP_HALT = 0x25,
};

// Memory Storage
#define MEMORY_MAX (1 << 16)
uint16_t memory[MEMORY_MAX];

// Registers
enum {
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC,
    R_COND,
    R_COUNT
};

// Register Storage
uint16_t reg[R_COUNT];

// Opcodes
enum {
    OP_BR = 0,
    OP_ADD, 
    OP_LD,
    OP_ST,
    OP_JSR,
    OP_AND,
    OP_LDR,
    OP_STR,
    OP_RTI,
    OP_NOT,
    OP_LDI,
    OP_STI,
    OP_JMP,
    OP_RES,
    OP_LEA,
    OP_TRAP
};

//Condition Flags
enum {
    FL_POS = 1 << 0,
    FL_ZRO = 1 << 1,
    FL_NEG = 1 << 2,
};

struct termios original_tio;

void disable_input_buffering() {
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;
    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering() {
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

uint16_t check_key() {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

void handle_interrupt(int signal) {
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

uint16_t sign_extend(uint16_t x, int bit_count) {
    if((x >> (bit_count -1)) & 1) {
        x |= (0xFFFF << bit_count);
    }
    return x;
}

uint16_t swap16(uint16_t x) {
    return (x << 8) | (x >> 8);
}

void update_flags(uint16_t r) {
    if(reg[r] == 0) {
        reg[R_COND] = FL_ZRO;
    } else if (reg[r] >> 15) {
        reg[R_COND] = FL_NEG;
    } else {
        reg[R_COND] = FL_POS;
    }
}

void read_image_file(FILE* file) {
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    uint16_t max_read = MEMORY_MAX - origin;
    uint16_t* p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    while(read-- > 0) {
        *p = swap16(*p);
        ++p;
    }
}

int read_image(const char* image_path) {
    FILE* file = fopen(image_path, "rb");
    if(!file) {return 0;};
    read_image_file(file);
    fclose(file);
    return 1;
}

void mem_write(uint16_t address, uint16_t val) {
    memory[address] = val;
}

uint16_t mem_read(uint16_t address) {
    if(address == MR_KBSR) {
        if(check_key()) {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        } else {
            memory[MR_KBSR] = 0;
        }
    }
    return memory[address];
}

void fn_op_add(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    uint16_t imm_flag = (instr >> 5) & 0x1;

    if(imm_flag) {
        uint16_t imm5 = sign_extend(instr & 0x1F, 5);
        reg[r0] = reg[r1] + imm5;
    } else {
        uint16_t r2 = instr & 0x7;
        reg[r0] = reg[r1] + reg[r2];
    }
    update_flags(r0);
}

void fn_op_and(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    uint16_t imm_flag = (instr >> 5) & 0x1;
    
    if(imm_flag) {
        uint16_t imm5 = sign_extend((instr&0x1F), 5);
        reg[r0] = reg[r1] & imm5;
    } else {
        uint16_t r2 = instr & 0x7;
        reg[r0] = reg[r1] & reg[r2];
    }
    update_flags(r0);
}

void fn_op_not(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    reg[r0] = ~reg[r1];
    update_flags(r0);
}

void fn_op_br(uint16_t instr) {
    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
    uint16_t cond_flag = (instr >> 9) & 0x7;
    if(cond_flag & reg[R_COND]) {
        reg[R_PC] += pc_offset;
    }
}

void fn_op_jmp(uint16_t instr) {
    uint16_t r1 = (instr >> 6) & 0x7;
    reg[R_PC] = reg[r1];
}

void fn_op_jsr(uint16_t instr) {
    reg[R_R7] = reg[R_PC];
    if ((instr >> 11) & 0x1 == 0) {
        uint16_t BaseR = (instr >> 6) & 0x7;
        reg[R_PC] = reg[BaseR];
    } else {
        reg[R_PC] = reg[R_PC] + sign_extend((instr & 0x7FF), 11);
    }
}

void fn_op_ld(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    reg[r0] = mem_read(reg[R_PC] + sign_extend((instr & 0x1FF), 9));
    update_flags(r0);
}

void fn_op_ldi(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
    reg[0] = mem_read(mem_read(reg[R_PC] + pc_offset));
    update_flags(r0);
}

void fn_op_ldr(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t BaseR = (instr >> 6) & 0x7;
    uint16_t pc_offset6 = instr &  0x3F;
    reg[r0] = mem_read(reg[BaseR] + sign_extend(pc_offset6, 6));
    update_flags(r0);
}

void fn_op_lea(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t pc_offset9 = instr & 0x1FF;
    reg[r0] = reg[R_PC] + sign_extend(pc_offset9, 9);
    update_flags(r0);
}

void fn_op_st(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t pc_offset9 = sign_extend(instr & 0x1FF, 9);
    mem_write(reg[R_PC] + pc_offset9, reg[r0]);
}

void fn_op_sti(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t pc_offset9 = sign_extend(instr & 0x1FF, 9);
    mem_write(mem_read(reg[R_PC] + pc_offset9), reg[r0]);
}

void fn_op_str(uint16_t instr) {
    uint16_t r0 = (instr >> 9) & 0x7;
    uint16_t r1 = (instr >> 6) & 0x7;
    uint16_t pc_offset6 = sign_extend(instr & 0x3F, 6);
    mem_write(reg[r1] + pc_offset6, reg[r0]);
}

int main(int argc, const char* argv[]) {

    if (argc < 2) {
        printf("lc3 [image-file1] ...\n");
        exit(2);
    }

    for(int j = 1; j < argc; ++j) {
        if(!read_image(argv[j])) {
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }

    // something called setup goes here
    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    reg[R_COND] = FL_ZRO;

    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;

    int running = 1;
    while(running) {
        uint16_t instr = mem_read(reg[R_PC]++);
        uint16_t op = instr >> 12;

        switch(op) {
            case OP_ADD:
                // Adding function call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // uint16_t r1 = (instr >> 6) & 0x7;
                // uint16_t imm_flag = (instr >> 5) & 0x1;

                // if(imm_flag) {
                //     uint16_t imm5 = sign_extend(instr & 0x1F, 5);
                //     reg[r0] = reg[r1] + imm5;
                // } else {
                //     uint16_t r2 = instr & 0x7;
                //     reg[r0] = reg[r1] + reg[r2];
                // }
                // update_flags(r0);
                fn_op_add(instr);
                break;
            case OP_AND:
                // AND func call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // uint16_t r1 = (instr >> 6) & 0x7;
                // uint16_t imm_flag = (instr >> 5) & 0x1;
                
                // if(imm_flag) {
                //     uint16_t imm5 = sign_extend((instr&0x1F), 5);
                //     reg[r0] = reg[r1] & imm5;
                // } else {
                //     uint16_t r2 = instr & 0x7;
                //     reg[r0] = reg[r1] & reg[r2];
                // }
                // update_flags(r0);
                fn_op_and(instr);
                break;
            case OP_NOT:
                // NOT func call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // uint16_t r1 = (instr >> 6) & 0x7;
                // reg[r0] = ~reg[r1];
                // update_flags(r0);
                fn_op_not(instr);
                break;
            case OP_BR:
                // BRANCH func call goes here
                // uint16_t n = (instr >> 11) & 0x1;
                // uint16_t z = (instr >> 10) & 0x1;
                // uint16_t p = (instr >> 9) & 0x1;
                // if((n & FL_NEG) || (z & FL_ZRO) || (p & FL_POS)) {
                //     reg[R_PC] = reg[R_PC] + sign_extend((instr & 0x1FF), 9);
                // }

                // uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                // uint16_t cond_flag = (instr >> 9) & 0x7;
                // if(cond_flag & reg[R_COND]) {
                //     reg[R_PC] += pc_offset;
                // }
                fn_op_br(instr);
                break;
            case OP_JMP:
                // JUMP func call goes here 
                // uint16_t r1 = (instr >> 6) & 0x7;
                // reg[R_PC] = reg[r1];
                fn_op_jmp(instr);
                break;
            case OP_JSR:
                // JUMP REG func call goes here
                // reg[R_R7] = reg[R_PC];
                // if ((instr >> 11) & 0x1 == 0) {
                //     uint16_t BaseR = (instr >> 6) & 0x7;
                //     reg[R_PC] = reg[BaseR];
                // } else {
                //     reg[R_PC] = reg[R_PC] + sign_extend((instr & 0x7FF), 11);
                // }

                fn_op_jsr(instr);
                break;
            case OP_LD:
                // LOAD func call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // reg[r0] = mem_read(reg[R_PC] + sign_extend((instr & 0x1FF), 9));
                // update_flags(r0);
                fn_op_ld(instr);
                break;
            case OP_LDI:
                // LOAD Indirect func call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                // reg[0] = mem_read(mem_read(reg[R_PC] + pc_offset));
                // update_flags(r0);
                fn_op_ldi(instr);
                break;
            case OP_LDR:
                // LOAD REG func call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // uint16_t BaseR = (instr >> 6) & 0x7;
                // uint16_t pc_offset6 = instr &  0x3F;
                // reg[r0] = mem_read(reg[BaseR] + sign_extend(pc_offset6, 6));
                // update_flags(r0);
                fn_op_ldr(instr);
                break;
            case OP_LEA:
                // Load Effective Area func call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // uint16_t pc_offset9 = instr & 0x1FF;
                // reg[r0] = reg[R_PC] + sign_extend(pc_offset9, 9);
                // update_flags(r0);
                fn_op_lea(instr);
                break;
            case OP_ST:
                //STORE func call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // uint16_t pc_offset9 = sign_extend(instr & 0x1FF, 9);
                // mem_write(reg[R_PC] + pc_offset, reg[r0]);
                fn_op_st(instr);
                break;
            case OP_STI:
                // STORE Indirect func call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // uint16_t pc_offset9 = sign_extend(instr & 0x1FF, 9);
                // mem_write(mem_read(reg[R_PC] + pc_offset9), reg[r0]);
                fn_op_sti(instr);
                break;
            case OP_STR:
                // STORE REG func call goes here
                // uint16_t r0 = (instr >> 9) & 0x7;
                // uint16_t r1 = (instr >> 6) & 0x7;
                // uint16_t pc_offset6 = sign_extend(instr & 0x3F, 6);
                // mem_write(reg[r1] + pc_offset6, reg[r0]);
                fn_op_str(instr);
                break;
            case OP_TRAP:
                reg[R_R7] = reg[R_PC];
                switch (instr & 0xFF) {
                    case TRAP_GETC:
                        // GETC code goes here
                        reg[R_R0] = (uint16_t)getchar();
                        update_flags(R_R0);
                        break;
                    case TRAP_OUT:
                        // OUT code goes here
                        putc((char)reg[R_R0], stdout);
                        fflush(stdout);
                        break;
                    case TRAP_PUTS:
                        // PUTS code goes here
                        uint16_t* c = memory + reg[R_R0];
                        while(*c) {
                            putc((char)*c, stdout);
                            ++c;
                        }
                        fflush(stdout);
                        break;
                    case TRAP_IN:
                        // IN code goes here
                        printf("Enter a character: ");
                        char ch = getchar();
                        putc(ch, stdout);
                        fflush(stdout);
                        reg[R_R0] = (uint16_t)c;
                        update_flags(R_R0);
                        break;
                    case TRAP_PUTSP:
                        // PUTSP code goes here
                        uint16_t* c1 = memory + reg[R_R0];
                        while(*c1) {
                            char char1 = (*c1) & 0xFF;
                            putc(char1, stdout);
                            char char2 = (*c1) >> 8;
                            if(char2) putc(char2, stdout);
                            ++c;
                        }
                        fflush(stdout);
                        break;
                    case TRAP_HALT:
                        // HALT code goes here
                        puts("HALT");
                        fflush(stdout);
                        running = 0;
                        break;
                }
                break;
            case OP_RES:
            case OP_RTI:
            default:
                // BAD Opcode Intimation goes here
                abort();
                break;
        }
    }
    // some kind of VM shutdown routine will be called here.
    restore_input_buffering();
}


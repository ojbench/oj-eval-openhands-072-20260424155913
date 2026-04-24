

#include <iostream>
#include <vector>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>

using namespace std;

// Simple RISC-V Interpreter (non-pipelined)
// Return value: Value in register x10 (a0)

class SimpleRISCVSimulator {
private:
    // Registers
    uint32_t regs[32];
    uint32_t pc;
    
    // Memory (simplified - 4KB)
    static const uint32_t MEMORY_SIZE = 4096;
    uint8_t memory[MEMORY_SIZE];
    
    // Simulation state
    bool halted;
    uint32_t instructions_executed;
    
public:
    SimpleRISCVSimulator() : halted(false), instructions_executed(0) {
        reset();
    }
    
    void reset() {
        memset(regs, 0, sizeof(regs));
        memset(memory, 0, sizeof(memory));
        pc = 0;
        halted = false;
        instructions_executed = 0;
    }
    
    // Load program from data file
    bool loadProgram(const string& filename) {
        ifstream file(filename);
        if (!file.is_open()) {
            cerr << "Error: Cannot open file " << filename << endl;
            return false;
        }
        
        string line;
        uint32_t address = 0;
        
        while (getline(file, line)) {
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') continue;
            
            // Parse hex values
            istringstream iss(line);
            uint32_t value;
            if (iss >> hex >> value) {
                if (address + 4 <= MEMORY_SIZE) {
                    // Store instruction in memory (little-endian)
                    memory[address] = value & 0xFF;
                    memory[address + 1] = (value >> 8) & 0xFF;
                    memory[address + 2] = (value >> 16) & 0xFF;
                    memory[address + 3] = (value >> 24) & 0xFF;
                    address += 4;
                }
            }
        }
        
        file.close();
        return true;
    }
    
    // Read 32-bit value from memory
    uint32_t readMemory(uint32_t address) {
        if (address + 4 <= MEMORY_SIZE) {
            return memory[address] | 
                   (memory[address + 1] << 8) |
                   (memory[address + 2] << 16) |
                   (memory[address + 3] << 24);
        }
        return 0;
    }
    
    // Write 32-bit value to memory
    void writeMemory(uint32_t address, uint32_t value) {
        if (address + 4 <= MEMORY_SIZE) {
            memory[address] = value & 0xFF;
            memory[address + 1] = (value >> 8) & 0xFF;
            memory[address + 2] = (value >> 16) & 0xFF;
            memory[address + 3] = (value >> 24) & 0xFF;
        }
    }
    
    // Execute one instruction
    void executeInstruction() {
        uint32_t instr = readMemory(pc);
        
        if (instr == 0) {
            halted = true;
            return;
        }
        
        uint32_t opcode = instr & 0x7F;
        uint32_t rd = (instr >> 7) & 0x1F;
        uint32_t funct3 = (instr >> 12) & 0x7;
        uint32_t rs1 = (instr >> 15) & 0x1F;
        uint32_t rs2 = (instr >> 20) & 0x1F;
        uint32_t funct7 = (instr >> 25) & 0x7F;
        
        // Extract immediate based on instruction type
        uint32_t imm = 0;
        if (opcode == 0x03 || opcode == 0x13 || opcode == 0x67) { // I-type
            imm = (instr >> 20) & 0xFFF;
            if (imm & 0x800) imm |= 0xFFFFF000; // Sign extend
        } else if (opcode == 0x23) { // S-type
            imm = ((instr >> 7) & 0x1F) | ((instr >> 25) << 5);
            if (imm & 0x800) imm |= 0xFFFFF000; // Sign extend
        } else if (opcode == 0x63) { // B-type
            imm = ((instr >> 7) & 0x1) | ((instr >> 8) & 0xF) << 1 | 
                  ((instr >> 25) & 0x3F) << 5 | ((instr >> 31) & 0x1) << 12;
            if (imm & 0x1000) imm |= 0xFFFFE000; // Sign extend
        } else if (opcode == 0x37) { // U-type
            imm = instr & 0xFFFFF000;
        } else if (opcode == 0x6F) { // J-type
            imm = ((instr >> 21) & 0x3FF) << 1 | ((instr >> 20) & 0x1) << 11 |
                  ((instr >> 12) & 0xFF) << 12 | ((instr >> 31) & 0x1) << 20;
            if (imm & 0x100000) imm |= 0xFFE00000; // Sign extend
        }
        
        // Execute instruction based on opcode
        switch (opcode) {
            case 0x03: // Load
                if (rd != 0) {
                    uint32_t addr = regs[rs1] + imm;
                    regs[rd] = readMemory(addr);
                }
                pc += 4;
                break;
                
            case 0x13: // Immediate arithmetic
                if (rd != 0) {
                    switch (funct3) {
                        case 0x0: regs[rd] = regs[rs1] + imm; break; // ADDI
                        case 0x4: regs[rd] = regs[rs1] ^ imm; break; // XORI
                        case 0x6: regs[rd] = regs[rs1] | imm; break; // ORI
                        case 0x7: regs[rd] = regs[rs1] & imm; break; // ANDI
                        case 0x1: regs[rd] = regs[rs1] << (imm & 0x1F); break; // SLLI
                        case 0x5:
                            if (funct7 == 0x0) regs[rd] = regs[rs1] >> (imm & 0x1F); // SRLI
                            else if (funct7 == 0x20) regs[rd] = int32_t(regs[rs1]) >> (imm & 0x1F); // SRAI
                            break;
                        case 0x2: regs[rd] = (int32_t(regs[rs1]) < int32_t(imm)) ? 1 : 0; break; // SLTI
                        case 0x3: regs[rd] = (regs[rs1] < imm) ? 1 : 0; break; // SLTIU
                    }
                }
                pc += 4;
                break;
                
            case 0x23: // Store
                {
                    uint32_t addr = regs[rs1] + imm;
                    writeMemory(addr, regs[rs2]);
                }
                pc += 4;
                break;
                
            case 0x33: // R-type arithmetic
                if (rd != 0) {
                    switch (funct3) {
                        case 0x0:
                            if (funct7 == 0x00) regs[rd] = regs[rs1] + regs[rs2]; // ADD
                            else if (funct7 == 0x20) regs[rd] = regs[rs1] - regs[rs2]; // SUB
                            break;
                        case 0x1: regs[rd] = regs[rs1] << (regs[rs2] & 0x1F); break; // SLL
                        case 0x2: regs[rd] = (int32_t(regs[rs1]) < int32_t(regs[rs2])) ? 1 : 0; break; // SLT
                        case 0x3: regs[rd] = (regs[rs1] < regs[rs2]) ? 1 : 0; break; // SLTU
                        case 0x4: regs[rd] = regs[rs1] ^ regs[rs2]; break; // XOR
                        case 0x5:
                            if (funct7 == 0x00) regs[rd] = regs[rs1] >> (regs[rs2] & 0x1F); // SRL
                            else if (funct7 == 0x20) regs[rd] = int32_t(regs[rs1]) >> (regs[rs2] & 0x1F); // SRA
                            break;
                        case 0x6: regs[rd] = regs[rs1] | regs[rs2]; break; // OR
                        case 0x7: regs[rd] = regs[rs1] & regs[rs2]; break; // AND
                    }
                }
                pc += 4;
                break;
                
            case 0x37: // LUI
                if (rd != 0) {
                    regs[rd] = imm;
                }
                pc += 4;
                break;
                
            case 0x17: // AUIPC
                if (rd != 0) {
                    regs[rd] = pc + imm;
                }
                pc += 4;
                break;
                
            case 0x63: // Branch
                {
                    bool take_branch = false;
                    switch (funct3) {
                        case 0x0: take_branch = (regs[rs1] == regs[rs2]); break; // BEQ
                        case 0x1: take_branch = (regs[rs1] != regs[rs2]); break; // BNE
                        case 0x4: take_branch = (int32_t(regs[rs1]) < int32_t(regs[rs2])); break; // BLT
                        case 0x5: take_branch = (int32_t(regs[rs1]) >= int32_t(regs[rs2])); break; // BGE
                    }
                    if (take_branch) {
                        pc = pc + imm;
                    } else {
                        pc += 4;
                    }
                }
                break;
                
            case 0x67: // JALR
                if (rd != 0) {
                    regs[rd] = pc + 4;
                }
                pc = (regs[rs1] + imm) & ~1;
                break;
                
            case 0x6F: // JAL
                if (rd != 0) {
                    regs[rd] = pc + 4;
                }
                pc = pc + imm;
                break;
                
            default:
                // Unknown instruction, halt
                halted = true;
                break;
        }
        
        instructions_executed++;
        
        // Safety check to prevent infinite loops
        if (instructions_executed > 100000) {
            halted = true;
        }
    }
    
    // Run simulation
    void run() {
        while (!halted && pc < MEMORY_SIZE) {
            executeInstruction();
        }
    }
    
    // Get register value
    uint32_t getRegister(uint32_t reg_num) const {
        if (reg_num < 32) {
            return regs[reg_num];
        }
        return 0;
    }
    
    // Get number of instructions executed
    uint32_t getInstructionsExecuted() const {
        return instructions_executed;
    }
};

int main() {
    SimpleRISCVSimulator simulator;
    
    // Read from standard input (data file)
    string content;
    string line;
    while (getline(cin, line)) {
        content += line + "\n";
    }
    
    // Create temporary file
    ofstream temp_file("temp_input.data");
    temp_file << content;
    temp_file.close();
    
    // Load and run program
    if (simulator.loadProgram("temp_input.data")) {
        simulator.run();
        // For a 5-stage pipeline, total cycles = instructions + pipeline fill + pipeline drain
        uint32_t instructions = simulator.getInstructionsExecuted();
        uint32_t cycles = instructions + 5 + 4; // 5 stages to fill, 4 stages to drain
        cout << cycles << endl;
    } else {
        cout << "0" << endl;
    }
    
    // Clean up
    remove("temp_input.data");
    
    return 0;
}


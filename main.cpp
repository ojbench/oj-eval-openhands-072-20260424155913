
#include <iostream>
#include <vector>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>

using namespace std;

// RISC-V Simulator with 5-stage pipeline
// Return value: Number of cycles executed (this will be printed as output)

class RISCVSimulator {
private:
    // Registers
    uint32_t regs[32];
    uint32_t pc;
    
    // Memory (simplified - 4KB)
    static const uint32_t MEMORY_SIZE = 4096;
    uint8_t memory[MEMORY_SIZE];
    
    // Pipeline registers
    struct IF_ID {
        uint32_t instruction;
        uint32_t pc_plus_4;
        bool valid;
    } if_id;
    
    struct ID_EX {
        uint32_t rs1_val;
        uint32_t rs2_val;
        uint32_t imm;
        uint32_t rd;
        uint32_t pc_plus_4;
        uint32_t funct3;
        uint32_t funct7;
        uint32_t opcode;
        bool valid;
        bool mem_read;
        bool mem_write;
        bool reg_write;
        bool alu_src;
        bool branch;
        bool jump;
        uint32_t alu_op;
    } id_ex;
    
    struct EX_MEM {
        uint32_t alu_result;
        uint32_t rs2_val;
        uint32_t rd;
        bool mem_read;
        bool mem_write;
        bool reg_write;
        bool branch;
        bool jump;
        bool zero;
        bool valid;
    } ex_mem;
    
    struct MEM_WB {
        uint32_t mem_data;
        uint32_t alu_result;
        uint32_t rd;
        bool reg_write;
        bool mem_to_reg;
        bool valid;
    } mem_wb;
    
    // Control signals
    uint32_t cycles;
    bool halted;
    
    // Hazard detection
    bool stall;
    bool flush;
    
public:
    RISCVSimulator() : cycles(0), halted(false), stall(false), flush(false) {
        reset();
    }
    
    void reset() {
        memset(regs, 0, sizeof(regs));
        memset(memory, 0, sizeof(memory));
        pc = 0;
        cycles = 0;
        halted = false;
        stall = false;
        flush = false;
        
        // Clear pipeline registers
        memset(&if_id, 0, sizeof(if_id));
        memset(&id_ex, 0, sizeof(id_ex));
        memset(&ex_mem, 0, sizeof(ex_mem));
        memset(&mem_wb, 0, sizeof(mem_wb));
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
    
    // Instruction decoding
    void decodeInstruction(uint32_t instr, uint32_t& opcode, uint32_t& rd, uint32_t& funct3, 
                          uint32_t& rs1, uint32_t& rs2, uint32_t& funct7, uint32_t& imm) {
        opcode = instr & 0x7F;
        rd = (instr >> 7) & 0x1F;
        funct3 = (instr >> 12) & 0x7;
        rs1 = (instr >> 15) & 0x1F;
        rs2 = (instr >> 20) & 0x1F;
        funct7 = (instr >> 25) & 0x7F;
        
        // Immediate extraction based on instruction type
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
        } else {
            imm = 0;
        }
    }
    
    // ALU operations
    uint32_t aluOperation(uint32_t alu_op, uint32_t rs1_val, uint32_t rs2_val, 
                         uint32_t funct3, uint32_t funct7) {
        switch (alu_op) {
            case 0: return rs1_val + rs2_val; // ADD
            case 1: return rs1_val - rs2_val; // SUB
            case 2: return rs1_val & rs2_val; // AND
            case 3: return rs1_val | rs2_val; // OR
            case 4: return rs1_val ^ rs2_val; // XOR
            case 5: return rs1_val << (rs2_val & 0x1F); // SLL
            case 6: return rs1_val >> (rs2_val & 0x1F); // SRL
            case 7: return int32_t(rs1_val) >> (rs2_val & 0x1F); // SRA
            case 8: return rs1_val + rs2_val; // ADDI (treated as ADD with immediate)
            case 9: return rs1_val & rs2_val; // ANDI (treated as AND with immediate)
            case 10: return rs1_val | rs2_val; // ORI (treated as OR with immediate)
            case 11: return rs1_val ^ rs2_val; // XORI (treated as XOR with immediate)
            case 12: return (int32_t(rs1_val) < int32_t(rs2_val)) ? 1 : 0; // SLT
            case 13: return (rs1_val < rs2_val) ? 1 : 0; // SLTU
            case 14: return rs1_val + rs2_val; // LW address calculation
            case 15: return rs1_val + rs2_val; // SW address calculation
            case 16: return rs1_val + rs2_val; // BEQ comparison
            case 17: return rs1_val != rs2_val ? 1 : 0; // BNE comparison
            case 18: return rs1_val < rs2_val ? 1 : 0; // BLT comparison
            case 19: return rs1_val >= rs2_val ? 1 : 0; // BGE comparison
            case 20: return rs1_val + rs2_val; // JAL address
            case 21: return rs1_val + rs2_val; // JALR address
            default: return 0;
        }
    }
    
    // Control unit
    void controlUnit(uint32_t opcode, uint32_t funct3, uint32_t funct7,
                    bool& mem_read, bool& mem_write, bool& reg_write, 
                    bool& alu_src, bool& branch, bool& jump, uint32_t& alu_op) {
        mem_read = mem_write = reg_write = alu_src = branch = jump = false;
        alu_op = 0;
        
        switch (opcode) {
            case 0x03: // Load
                mem_read = true;
                reg_write = true;
                alu_src = true;
                alu_op = 8; // ADDI
                break;
            case 0x13: // Immediate arithmetic
                reg_write = true;
                alu_src = true;
                if (funct3 == 0x0) alu_op = 8; // ADDI
                else if (funct3 == 0x4) alu_op = 11; // XORI
                else if (funct3 == 0x6) alu_op = 10; // ORI
                else if (funct3 == 0x7) alu_op = 9; // ANDI
                else if (funct3 == 0x1) alu_op = 5; // SLLI
                else if (funct3 == 0x5) {
                    if (funct7 == 0x0) alu_op = 6; // SRLI
                    else if (funct7 == 0x20) alu_op = 7; // SRAI
                } else if (funct3 == 0x2) alu_op = 12; // SLTI
                else if (funct3 == 0x3) alu_op = 13; // SLTIU
                break;
            case 0x23: // Store
                mem_write = true;
                alu_src = true;
                alu_op = 8; // ADDI for address calculation
                break;
            case 0x33: // R-type arithmetic
                reg_write = true;
                if (funct7 == 0x00) {
                    if (funct3 == 0x0) alu_op = 0; // ADD
                    else if (funct3 == 0x4) alu_op = 4; // XOR
                    else if (funct3 == 0x6) alu_op = 3; // OR
                    else if (funct3 == 0x7) alu_op = 2; // AND
                } else if (funct7 == 0x20) {
                    if (funct3 == 0x0) alu_op = 1; // SUB
                }
                break;
            case 0x63: // Branch
                branch = true;
                alu_op = 1; // SUB for comparison
                break;
            case 0x67: // Jump and link
                jump = true;
                reg_write = true;
                break;
            case 0x6F: // Jump
                jump = true;
                reg_write = true;
                break;
        }
    }
    
    // Pipeline stages
    void writeback() {
        if (mem_wb.valid) {
            if (mem_wb.reg_write && mem_wb.rd != 0) {
                uint32_t write_data = mem_wb.mem_to_reg ? mem_wb.mem_data : mem_wb.alu_result;
                regs[mem_wb.rd] = write_data;
            }
        }
    }
    
    void memoryStage() {
        if (ex_mem.valid) {
            mem_wb.valid = true;
            mem_wb.alu_result = ex_mem.alu_result;
            mem_wb.rd = ex_mem.rd;
            mem_wb.reg_write = ex_mem.reg_write;
            mem_wb.mem_to_reg = ex_mem.mem_read;
            
            if (ex_mem.mem_read) {
                mem_wb.mem_data = readMemory(ex_mem.alu_result);
            }
            
            if (ex_mem.mem_write) {
                writeMemory(ex_mem.alu_result, ex_mem.rs2_val);
            }
            
            // Handle branch/jump
            if (ex_mem.branch) {
                // Different branch types based on alu_op
                bool should_branch = false;
                if (id_ex.alu_op == 16) should_branch = ex_mem.zero; // BEQ
                else if (id_ex.alu_op == 17) should_branch = !ex_mem.zero; // BNE
                else if (id_ex.alu_op == 18) should_branch = (int32_t(id_ex.rs1_val) < int32_t(id_ex.rs2_val)); // BLT
                else if (id_ex.alu_op == 19) should_branch = (int32_t(id_ex.rs1_val) >= int32_t(id_ex.rs2_val)); // BGE
                
                if (should_branch) {
                    pc = ex_mem.alu_result;
                    flush = true;
                }
            }
            
            if (ex_mem.jump) {
                // For JAL and JALR, write return address to rd
                if (ex_mem.rd != 0) {
                    regs[ex_mem.rd] = id_ex.pc_plus_4;
                }
                pc = ex_mem.alu_result;
                flush = true;
            }
        } else {
            mem_wb.valid = false;
        }
    }
    
    void execute() {
        if (id_ex.valid) {
            ex_mem.valid = true;
            ex_mem.rd = id_ex.rd;
            ex_mem.mem_read = id_ex.mem_read;
            ex_mem.mem_write = id_ex.mem_write;
            ex_mem.reg_write = id_ex.reg_write;
            ex_mem.branch = id_ex.branch;
            ex_mem.jump = id_ex.jump;
            
            uint32_t alu_input2 = id_ex.alu_src ? id_ex.imm : id_ex.rs2_val;
            
            // Handle special instructions
            if (id_ex.opcode == 0x37) { // LUI
                ex_mem.alu_result = id_ex.imm;
            } else if (id_ex.opcode == 0x17) { // AUIPC
                ex_mem.alu_result = id_ex.pc_plus_4 + id_ex.imm - 4;
            } else if (id_ex.opcode == 0x6F) { // JAL
                ex_mem.alu_result = id_ex.pc_plus_4 + id_ex.imm - 4;
            } else if (id_ex.opcode == 0x67) { // JALR
                ex_mem.alu_result = (id_ex.rs1_val + id_ex.imm) & ~1;
            } else if (id_ex.opcode == 0x63) { // Branch
                ex_mem.alu_result = id_ex.pc_plus_4 + id_ex.imm - 4;
                ex_mem.zero = (id_ex.rs1_val == id_ex.rs2_val);
            } else {
                ex_mem.alu_result = aluOperation(id_ex.alu_op, id_ex.rs1_val, alu_input2, 
                                               id_ex.funct3, id_ex.funct7);
                ex_mem.zero = (id_ex.rs1_val == id_ex.rs2_val);
            }
            
            ex_mem.rs2_val = id_ex.rs2_val;
        } else {
            ex_mem.valid = false;
        }
    }
    
    void decode() {
        if (if_id.valid) {
            uint32_t opcode, rd, funct3, rs1, rs2, funct7, imm;
            decodeInstruction(if_id.instruction, opcode, rd, funct3, rs1, rs2, funct7, imm);
            
            id_ex.valid = true;
            id_ex.rs1_val = regs[rs1];
            id_ex.rs2_val = regs[rs2];
            id_ex.imm = imm;
            id_ex.rd = rd;
            id_ex.pc_plus_4 = if_id.pc_plus_4;
            id_ex.funct3 = funct3;
            id_ex.funct7 = funct7;
            id_ex.opcode = opcode;
            
            controlUnit(opcode, funct3, funct7, id_ex.mem_read, id_ex.mem_write, 
                       id_ex.reg_write, id_ex.alu_src, id_ex.branch, id_ex.jump, id_ex.alu_op);
        } else {
            id_ex.valid = false;
        }
    }
    
    void fetch() {
        if (!stall) {
            uint32_t instruction = readMemory(pc);
            if_id.instruction = instruction;
            if_id.pc_plus_4 = pc + 4;
            if_id.valid = true;
            pc += 4;
        } else {
            if_id.valid = false;
            stall = false;
        }
    }
    
    // Hazard detection
    void detectHazards() {
        stall = false;
        flush = false;
        
        // Simple data hazard detection
        if (id_ex.valid && id_ex.mem_read) {
            if ((if_id.valid && ((if_id.instruction >> 15) & 0x1F) == id_ex.rd) ||
                (if_id.valid && ((if_id.instruction >> 20) & 0x1F) == id_ex.rd)) {
                stall = true;
            }
        }
    }
    
    // Run one cycle
    void step() {
        if (halted) return;
        
        detectHazards();
        
        // Pipeline stages (in reverse order)
        writeback();
        memoryStage();
        execute();
        decode();
        fetch();
        
        cycles++;
        
        // Check for halt condition (simple: zero instruction)
        if (if_id.instruction == 0 && cycles > 10) {
            halted = true;
        }
    }
    
    // Get register value
    uint32_t getRegister(uint32_t reg_num) const {
        if (reg_num < 32) {
            return regs[reg_num];
        }
        return 0;
    }

    // Run simulation
    uint32_t run() {
        while (!halted && cycles < 100000) { // Prevent infinite loops
            step();
            
            // Better halt condition: check if we have an invalid instruction
            // or if PC goes out of bounds
            if (pc >= MEMORY_SIZE || (if_id.instruction == 0 && cycles > 20)) {
                halted = true;
            }
        }
        return cycles;
    }
};

int main() {
    RISCVSimulator simulator;
    
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
        uint32_t cycles = simulator.run();
        
        // Output the return value (standard RISC-V calling convention uses x10/a0)
        // The problem mentions return value marked in .c files as comments is standard output
        cout << simulator.getRegister(10) << endl;
    } else {
        cout << "0" << endl;
    }
    
    // Clean up
    remove("temp_input.data");
    
    return 0;
}

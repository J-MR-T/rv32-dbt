#include <err.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <array>
#include <fstream>
#include <elf.h>
#include <iostream>
#include <sstream>
#include <regex>

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/TargetSelect.h>

#include <frvdec.h>

#ifndef NDEBUG
#define DEBUGLOG(x, ...) {llvm::errs() << x; __VA_ARGS__; llvm::errs() << "\n", fflush(stderr);}
#define IFDEBUG(x) x
#define IFDEBUGELSE(x, y) x

#else

#define DEBUGLOG(x, ...)
#define IFDEBUG(x)
#define IFDEBUGELSE(x, y) y

#endif


#define STRINGIZE(x) #x
#define STRINGIZE_MACRO(x) STRINGIZE(x)

// search for "HW11 start" to find the start of the relevant code

// exit status 2 for 2do :)
#define EXIT_TODO                                                                      \
    llvm::errs() << "TODO(Line " STRINGIZE_MACRO(__LINE__) "): Not implemented yet\n"; \
    exit(2);

#define EXIT_TODO_X(x)                                                         \
    llvm::errs() << "TODO(Line " STRINGIZE_MACRO(__LINE__) "): " << x << "\n"; \
    exit(2);

using ChunkFunc = uint32_t(uint32_t* regs);

namespace color{
    const char* red = "\033[0;31m";
    const char* green = "\033[0;32m";
    const char* yellow = "\033[0;33m";
    const char* blue = "\033[0;34m";
    const char* magenta = "\033[0;35m";
    const char* cyan = "\033[0;36m";
    const char* white = "\033[0;37m";
    const char* reset = "\033[0m";
}

namespace ArgParse{
    struct Arg{
        std::string shortOpt{""};
        std::string longOpt{""};
        uint32_t pos{0}; //if 0, no positional arg
        std::string description{""};
        bool required{false};
        bool flag{false};


        //define < operator, necessary for map
        bool operator<(const Arg& other) const{
            return shortOpt < other.shortOpt;
        }
    };

    std::map<Arg, std::string> parsedArgs{};
    
    // struct for all possible arguments
    const struct {
        const Arg help{      "h", "help"      , 0, "Show this help message and exit"                                                                     , false, true};
        const Arg input{     "i", "input"     , 1, "Input executable. Must be ELF EXEC. Loading of shared libraries is not performed."                   ,  true, false};
        const Arg loadAddr{  "l", "load-addr" , 0, "Load the binary at the specified (hex) address in the guest memory"                                  , false, false};
        const Arg step{      "s", "step"      , 0, "Step through the binary, with options for printing and previewing instructions"                      , false, true};

        const Arg sentinel{"", "", 0, "", false, false};

        const llvm::SmallVector<const Arg*,16> all = {&help, &input, &loadAddr, &step, &sentinel};
        
        // iterator over all
        const Arg* begin() const{
            return all[0];
        }

        const Arg* end() const{
            return &sentinel;
        }
    } possible;

    void printHelp(){
        std::cerr << "A dynamic binary translator for rv32i" << std::endl;
        std::cerr << "Usage: " << std::endl;
        for(auto& arg:possible){
            std::cerr << "  ";
            if(arg.shortOpt != ""){
                std::cerr << "-" << arg.shortOpt;
            }
            if(arg.longOpt != ""){
                if(arg.shortOpt != ""){
                    std::cerr << ", ";
                }
                std::cerr << "--" << arg.longOpt;
            }
            if(arg.pos != 0){
                std::cerr << " (or positional, at position " << arg.pos << ")";
            }else if(arg.flag){
                std::cerr << " (flag)";
            }
            std::cerr << std::endl;
            std::cerr << "    " << arg.description << std::endl;
        }

        std::cerr << std::endl;

        std::cerr << "Because no loading of shared libraries is performed, compiling with -nostdlib makes most sense.\nThe toolchain that this program was tested with was the riscv32-unknown-elf-gcc compiler,\nand qemu-riscv32 for verification." << std::endl;

        std::cerr << "Examples: " << std::endl;
        std::cerr << "  "         << "main -i exec" << std::endl;
        std::cerr << "  "         << "main exec" << std::endl;
        std::cerr << "  "         << "main -s exec" << std::endl;
    }

    //unordered_map doesnt work because of hash reasons (i think), so just define <, use ordered
    std::map<Arg, std::string>& parse(int argc, char *argv[]){
        std::stringstream ss;
        ss << " ";
        for (int i = 1; i < argc; ++i) {
            ss << argv[i] << " ";
        }

        std::string argString = ss.str();


        //handle positinoal args first, they have lower precedence
        //find them all, put them into a vector, then match them to the possible args
        uint32_t actualPositon = 1;
        std::vector<std::string> positionalArgs{};
        for(int i = 1; i < argc; ++i){
            for(const auto& arg : possible){
                if(!arg.flag && (("-"+arg.shortOpt) == std::string{argv[i-1]} || ("--"+arg.longOpt) == std::string{argv[i-1]})){
                    //the current arg is the value to another argument, so we dont count it
                    goto cont;
                }
            }


            if(argv[i][0] != '-'){
                // now we know its a positional arg
                positionalArgs.emplace_back(argv[i]);
                actualPositon++;
            }
cont:
            continue;
        }

        for(const auto& arg : possible){
            if(arg.pos != 0){
                //this is a positional arg
                if(positionalArgs.size() > arg.pos-1){
                    parsedArgs[arg] = positionalArgs[arg.pos-1];
                }
            }
        }

        bool missingRequired = false;

        //long/short/flags
        for(const auto& arg : possible){
            if(!arg.flag){
                std::regex matchShort{" -"+arg.shortOpt+"\\s*([^\\s]+)"};
                std::regex matchLong{" --"+arg.longOpt+"(\\s*|=)([^\\s=]+)"};
                std::smatch match;
                if(arg.shortOpt!="" && std::regex_search(argString, match, matchShort)){
                    parsedArgs[arg] = match[1];
                }else if(arg.longOpt!="" && std::regex_search(argString, match, matchLong)){
                    parsedArgs[arg] = match[2];
                }else if(arg.required && !parsedArgs.contains(arg)){
                    std::cerr << "Missing required argument: -" << arg.shortOpt << "/--" << arg.longOpt << std::endl;
                    missingRequired = true;
                }
            }else{
                std::regex matchFlagShort{" -[a-zA-z]*"+arg.shortOpt};
                std::regex matchFlagLong{" --"+arg.longOpt};
                if(std::regex_search(argString, matchFlagShort) || std::regex_search(argString, matchFlagLong)){
                    parsedArgs[arg] = ""; // empty string for flags, will just be checked using .contains
                }
            };
        }

        if(missingRequired){
            printHelp();
            exit(EXIT_FAILURE);
        }
        return parsedArgs;
    }

}

// the only useful thing GRNVS ever gave us (/s) (well except that it's technically partly unsafe. But ignore that)

char * hexdump2(const void *buffer, ssize_t len, size_t addressDisplayOffset = 0) {
	static char str[65536];
	unsigned int i;
	int p = 0;

	for (i = 0; i < len; i++) {
		if ((i % 8 == 0) && (i != 0)) {
			p += sprintf(str + p, "  ");
		}
		if (i % 16 == 0) {
			if (i == 0) {
				p += sprintf(str + p, ">> %.8lx:  ", i+addressDisplayOffset);
			} else {
				str[p++] = '\n';
				p += sprintf(str + p, ">> %.8lx:  ", i+addressDisplayOffset);
			}
		}
		p += sprintf(str + p, "%.2x ", ((unsigned char *) buffer)[i]);

		if ((sizeof(str) - p) < 64)
			break;
	}

	return str;
}

void hexdump(const void *buffer, ssize_t len, size_t addressDisplayOffset = 0) {
	char *str = hexdump2(buffer, len, addressDisplayOffset);
	fprintf(stderr, "%s\n", str);
}


// HW11 start
/*
Analysis: 
- sample programs can be found in samples/mini.c
- I included a binary just in case, compiled with `riscv32-unknown-elf-gcc -nostdlib mini.c -o mini --sysroot=/usr/riscv32-unknown-elf -march=rv32id` (Makefile for mini also included)
- performance testing was done using `time` (the zsh builtin, not the program), as the performance comparison was always to qemu which was tested the same way, and a single run was long enough, that the results were not skewed by more then a few percent. So take the performance numbers with a grain of salt, consider them right in the ballpark, but not exact. Only the time reportedly spent in user mode was considered. The program was compiled on -O3 for the performance tests

Performance tests (`mine` for my translator, 10 iterations):
- mini.c multest:
    - qemu: 0.659s
    - mine: 1.670s

Possible performance improvements:
- use chaining, at least for the same function
    on multest perf reports, that about 55.4% of the time was spent in the run method, meaning the absolute maximum performance gain of chaining everything (which is not possible, because of the limitations talked about in the lecture) would be reducing the programs runtime to about 45% (keep in mind, as a sampling profiler perf is not very accurate, so this number is to be taken with lots of grains of salt) of what it was before. In the run method itself, comparing the values for equality took the most time, while computing the hash comes in second.
- use a custom hashmap, adapted to pointers as values and integer keys
    judging from the performance results above, this might also reduce overhead, without chaining
- basic blocks can be quite small, so superblocks could also help to reduce overhead
- adaptive compilation: track how often chunks are called and use llvm to optimize highly used chunks
    i tested multest again, but with aggressive optimization by llvm (but regardless of how often a chunk was called, just as a rough approximation of the maximum performance benefit that could be gained), my implementation then took 0.918s. This is about 55% of the time it took before. This can be considered the theoretical performance cealing achievable with adaptive compilation.



*/
// dynamic binary translator for rv32i

llvm::Value* hostPointerToLLVMPtr(llvm::IRBuilder<>& irb, void* ptr){
    return irb.CreateIntToPtr(irb.getInt64(reinterpret_cast<uint64_t>(ptr)), irb.getPtrTy());
}

/// Compile function from module; module is consumed; returns nullptr on failure.
template<llvm::CodeGenOpt::Level OptLevel = llvm::CodeGenOpt::None>
void* compile(std::unique_ptr<llvm::Module> mod, const std::string& name) {
    std::string error;
    llvm::TargetOptions options;

    llvm::EngineBuilder builder(std::move(mod));
    builder.setEngineKind(llvm::EngineKind::JIT);
    builder.setErrorStr(&error);
    builder.setOptLevel(OptLevel);
    builder.setTargetOptions(options);

    llvm::ExecutionEngine* engine = builder.create();
    if (!engine)
        err(1, "could not create engine: %s", error.c_str());

    return reinterpret_cast<void*>(engine->getFunctionAddress(name));
}


/// overestimate/overassume the size of the memory area to be protected, so that this can be called with any address, regardless of page boundary alignment
int pageSafeMprotect (void *__addr, size_t __len, int __prot) __THROW {
    DEBUGLOG("pagesize: " << getpagesize());
    // align to page boundary
    void* addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(__addr) & ~(getpagesize()-1));
    assert(addr <= __addr);
    assert(reinterpret_cast<uintptr_t>(addr) % getpagesize() == 0);
    size_t len = __len + (reinterpret_cast<uintptr_t>(__addr) - reinterpret_cast<uintptr_t>(addr)); // __addr always >= addr -> this rounds up the length to a page boundary
    assert(len >= __len);
    DEBUGLOG("mprotect addresses from " << addr << " to " << reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr)+len));
    return mprotect(addr, len, __prot);
}

class DBT {
public:
    uint64_t memSize = 0x100000000; // 4 GiB
    void* guestMem;
    void* programStart;
    size_t programLength_b; // in bytes
    uint32_t entryPointFileOffset; // where the entry point starts in the file
    uint32_t stackSize = 0x000010000; // 64 KiB stack
    Elf32_Ehdr elfHeader;

    static constexpr unsigned instructionSize = 4;

    llvm::DenseMap<uintptr_t /* pc */, ChunkFunc (*) /* code */> codeCache{};

    DBT(uint64_t memSize, uint32_t programLoadOffset, const std::string& filename) : memSize(memSize){
        // allocate 4GiB of memory as guest memory
        guestMem = mmap(NULL, memSize, PROT_NONE, MAP_PRIVATE|MAP_ANON|MAP_NORESERVE, -1, 0);
        DEBUGLOG("mmapped addresses from " << guestMem << " to " << reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(guestMem)+memSize) << " (size: " << memSize << " bytes");
        if(guestMem == MAP_FAILED)
            err(EXIT_FAILURE, "could not allocate guest memory");

        // load binary into guest memory
        std::ifstream file(filename, std::ios_base::in | std::ios::binary);
        if(!file)
            err(EXIT_FAILURE, "could not open file %s", filename.c_str());

        // get filesize
        file.seekg(0, std::ios::end);
        programLength_b = file.tellg();
        file.seekg(0, std::ios::beg);

        programStart = reinterpret_cast<uint8_t*>(guestMem)+programLoadOffset;
        if(reinterpret_cast<uint8_t*>(programStart)+programLength_b > reinterpret_cast<uint8_t*>(guestMem)+memSize)
            errx(EXIT_FAILURE, "Cannot load a program that large that far back in memory.");

        auto ret = pageSafeMprotect(programStart, programLength_b, PROT_READ|PROT_WRITE);
        if(ret != 0)
            err(EXIT_FAILURE, "mprotect failed");
        file.read(reinterpret_cast<char*>(programStart), programLength_b);
        ret = pageSafeMprotect(programStart, programLength_b, PROT_READ);
        if(ret != 0)
            err(EXIT_FAILURE, "mprotect failed");

        // read elf header
        DEBUGLOG("sizeof elf header: " << sizeof(elfHeader) << " bytes" << ", program length: " << programLength_b << " bytes");
        if(sizeof(elfHeader) > programLength_b)
            errx(EXIT_FAILURE, "file %s too small to contain elf header", filename.c_str());

        memcpy(&elfHeader, programStart, sizeof(elfHeader));

        if(elfHeader.e_ident[EI_MAG0] != ELFMAG0 || elfHeader.e_ident[EI_MAG1] != ELFMAG1 || elfHeader.e_ident[EI_MAG2] != ELFMAG2 || elfHeader.e_ident[EI_MAG3] != ELFMAG3)
            errx(EXIT_FAILURE, "file %s is not an elf file", filename.c_str());
        
        if(elfHeader.e_type != ET_EXEC)
            errx(EXIT_FAILURE, "file %s is not an elf executable file", filename.c_str());
        
        if(elfHeader.e_machine != EM_RISCV)
            errx(EXIT_FAILURE, "file %s is not a riscv elf file", filename.c_str());

        DEBUGLOG("elf file successfully loaded into memory at " << programStart);

        // dump elf header
        DEBUGLOG("elf header:");
        DEBUGLOG("\te_ident: "     << hexdump2(elfHeader.e_ident, EI_NIDENT));
        DEBUGLOG("\te_type: "      << elfHeader.e_type);
        DEBUGLOG("\te_machine: "   << elfHeader.e_machine);
        DEBUGLOG("\te_version: "   << elfHeader.e_version);
        DEBUGLOG("\te_entry: "     << elfHeader.e_entry);
        DEBUGLOG("\te_phoff: "     << elfHeader.e_phoff);
        DEBUGLOG("\te_shoff: "     << elfHeader.e_shoff);
        DEBUGLOG("\te_flags: "     << elfHeader.e_flags);
        DEBUGLOG("\te_ehsize: "    << elfHeader.e_ehsize);
        DEBUGLOG("\te_phentsize: " << elfHeader.e_phentsize);
        DEBUGLOG("\te_phnum: "     << elfHeader.e_phnum);
        DEBUGLOG("\te_shentsize: " << elfHeader.e_shentsize);
        DEBUGLOG("\te_shnum: "     << elfHeader.e_shnum);
        DEBUGLOG("\te_shstrndx: "  << elfHeader.e_shstrndx);

        // load section headers
        llvm::SmallVector<Elf32_Shdr, 16> sectionHeaders(elfHeader.e_shnum);
        memcpy(sectionHeaders.data(), reinterpret_cast<uint8_t*>(programStart)+elfHeader.e_shoff, elfHeader.e_shnum*sizeof(Elf32_Shdr));

        // load program headers
        llvm::SmallVector<Elf32_Phdr, 8> programHeaders(elfHeader.e_phnum);
        memcpy(programHeaders.data(), reinterpret_cast<uint8_t*>(programStart)+elfHeader.e_phoff, elfHeader.e_phnum*sizeof(Elf32_Phdr));

        DEBUGLOG("successfully loaded " << elfHeader.e_phnum << " program headers and " << elfHeader.e_shnum << " section headers");

        // find first LOAD in program headers, in order to find where the text section starts in the file
        // i bet there is a way simpler way to do this, but I couldn't find it :(
        Elf32_Phdr* firstLoad = nullptr;
        for(unsigned i=0; i<elfHeader.e_phnum; i++){
            if(programHeaders[i].p_type == PT_LOAD){
                firstLoad = &programHeaders[i];
                break;
            }
        }
        if(firstLoad == nullptr)
            errx(EXIT_FAILURE, "no PT_LOAD program header found");

        if(firstLoad->p_flags != (PF_R|PF_X))
            errx(EXIT_FAILURE, "first PT_LOAD program header has wrong flags");

        entryPointFileOffset = elfHeader.e_entry - firstLoad->p_vaddr;
        DEBUGLOG("Found entry point at: " << reinterpret_cast<void*>(entryPointFileOffset));
    }

    template<bool step>
    void emulate(){
        // init stack using mprotect
        auto stackStart = reinterpret_cast<uint8_t*>(guestMem)+static_cast<uintptr_t>(memSize)-static_cast<uintptr_t>(stackSize);
        DEBUGLOG("Smallest host addr: " << guestMem);
        DEBUGLOG("Highest host addr: " << reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(guestMem)+memSize));
        DEBUGLOG("Stack starts at host addr " << reinterpret_cast<void*>(stackStart));
        auto ret = pageSafeMprotect(stackStart, stackSize, PROT_READ|PROT_WRITE);
        if(ret != 0)
            err(EXIT_FAILURE, "mprotect failed");


        // make this configurable
        //uintptr_t loadAddr =

        std::array<uint32_t, 32> regs = {0};
        // initialize registers correctly (r2 == sp, right? etc.)
        regs[2] = memSize-16;// stack pointer starts at top of stack, stack is end of guest memory

        auto stackPointerHost = reinterpret_cast<uintptr_t>(guestMem)+regs[2];
        (void) stackPointerHost;
        DEBUGLOG("Stack pointer is at host address " << reinterpret_cast<void*>(stackPointerHost));

        run<step>(entryPointFileOffset, regs.data(), regs.size());
    }

    template<bool step>
    void run(uintptr_t pc, uint32_t* regs, uint32_t numRegs){
        while(true){
            ChunkFunc* translated;
            if(auto it = codeCache.find(pc); it == codeCache.end()){
                // translate code
                translated = translate<step>(pc, step?1:64);
                codeCache[pc] = translated;
            } else {
                translated = codeCache[pc];
            }

            if constexpr(step)
                llvm::outs() << color::red << "Executing at " << reinterpret_cast<void*>(pc) << "..." << color::reset << "\n";
            pc = translated(regs);
            if constexpr (step){
menu:
                llvm::outs() << color::red << "Next instruction: " << color::reset << "\n";
                decodeChunk<true>(pc, 1);
                llvm::outs() << "Command ('h' for help): ";
                char c = getchar();
                llvm::outs() << "\n";
                bool runMenu = true;

                if(c == 'q' || c == EOF)
                    break;
                else if(c == 'i'){
                    DEBUGLOG("value of regs pointer: " << regs);
                    for (unsigned i = 0; i < numRegs; ++i) {
                        if(regs[i]!=0){
                            llvm::outs() << "r" << i << " = 0x"; llvm::outs().write_hex(regs[i]) << " = " << regs[i];
                            if(regs[i] & 0x80000000)
                                llvm::outs() << " = -" << (~regs[i]+1);
                            llvm::outs() << "\n";
                        }
                    }
                    llvm::outs() << "pc: 0x"; llvm::outs().write_hex(pc) << "\n";
                    llvm::outs() << "All other registers are 0\n";
                }else if(c == 's'){
                    runMenu = false;
                }else if(c == 'r'){
                    run<false>(pc, regs, numRegs);
                    return;
                }else if(c == 'p')
                    decodeChunk<true>(pc, 4);
                else if(c == 'h')
                    llvm::outs() << "Help:\n\ts: step one instruction\n\tr: exit stepping mode and run normally\n\tq: quit\n\ti: print register info\n\tp: preview the next 4 instructions without executing them\n";
                else if(c == '\n')
                    goto menu;
                else
                    llvm::outs() << "Unknown command\n";

                // get another char to get rid of newline
                if(char c = getchar(); c != '\n') ungetc(c, stdin);

                if(runMenu) goto menu;
            }
        }
    }


    static void syscall(uint32_t* regs, void* guestMem){
        DEBUGLOG("syscall called!");
        for(unsigned i=0; i<32; i++)
            DEBUGLOG("r" << i << ": " << regs[i]);

        switch(regs[17]){
            case 64: // write
                {
                char* buf = reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(guestMem)+regs[11]);
                DEBUGLOG("calling write with fd " << regs[10] << ", buf " << reinterpret_cast<void*>(buf) << ", count " << regs[12]);
                write(regs[10], buf, regs[12]);
                }
                break;
            case 93: // exit
                DEBUGLOG("calling exit with code " << regs[10]);
                exit(regs[10]);
                break;
            default:
                errx(EXIT_FAILURE, "unhandled syscall %d", regs[17]);
        }
    }

    template<bool print = false>
    std::tuple<unsigned, llvm::SmallVector<FrvInst, 16>, bool> decodeChunk(uintptr_t pc, unsigned maxInstructionsPerChunk = 16){
        // find basic block
        // Translate in 64 byte (16 instruction) chunks at once, until a jump is found
        llvm::SmallVector<FrvInst, 16> insts(maxInstructionsPerChunk);

        // find basic block (-> stop at jump), max bb size 64 bytes/16 instructions
        unsigned numInsts = 0;
        bool foundJump{false};
        for (auto& inst : insts){
            frv_decode(sizeof(FrvInst), reinterpret_cast<const uint8_t*>(programStart)+pc+((numInsts++)*instructionSize), FRV_RV32, &inst);

            if constexpr (print){
                char buf[4096] = {0};
                frv_format(&inst, sizeof(buf)-1, buf);
                printf(">> %.8lx:  %s\n", pc+((numInsts-1)*instructionSize), buf);
            }

            if(inst.mnem >= FRV_JAL && inst.mnem <= FRV_BGEU){ // see frvdec.h, all branch/jump instructions are in this range
                foundJump = true;
                break;
            }
        }
        return {numInsts, std::move(insts), foundJump};
    }

    template<bool print = false>
    ChunkFunc* translate(uintptr_t pc, unsigned maxInstructionsPerChunk = 16){
        // TODO check if need to translate, or cached

        if constexpr(print) llvm::outs() << color::red << "Translating Instruction(s):" << color::reset << "\n";
        auto [numInsts, insts, foundJump] = decodeChunk<print>(pc, maxInstructionsPerChunk);
#ifndef NDEBUG
            hexdump(reinterpret_cast<const uint8_t*>(programStart)+pc, insts.size()*instructionSize, pc);
#endif


        llvm::LLVMContext ctx;
        auto modUP = std::make_unique<llvm::Module>("rv32i", ctx);
        llvm::IRBuilder<> irb(ctx);



        // translated functions get ptr to register buffer as arg, return next pc
        llvm::Function* func = llvm::Function::Create(
            llvm::FunctionType::get(
                irb.getInt32Ty(),
                {irb.getPtrTy()},
                false
            ),
            llvm::Function::ExternalLinkage,
            "trans_0x"+llvm::Twine::utohexstr(pc),
            modUP.get()
        );
        irb.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", func));

        auto getReg = [&irb, func](int reg) -> llvm::Value* {
            assert(reg != FRV_REG_INV && "got invalid register");

            if(reg == 0) [[unlikely]] return irb.getInt32(0);

            auto* regs = func->arg_begin();
            auto gep = irb.CreateGEP(irb.getInt32Ty(), regs, {irb.getInt32(reg)},
                IFDEBUGELSE("ptrToR"+std::to_string(reg)+"_", ""));
            return irb.CreateLoad(irb.getInt32Ty(), gep,
                IFDEBUGELSE("r"+std::to_string(reg)+"_", ""));
        };

        auto setReg = [&irb, func](int reg, llvm::Value* val) -> void {
            assert(reg != FRV_REG_INV && "got invalid register");

            if(reg == 0) [[unlikely]] return;

            auto* regs = func->arg_begin();
            auto gep = irb.CreateGEP(irb.getInt32Ty(), regs, {irb.getInt32(reg)},
                IFDEBUGELSE("ptrToR"+std::to_string(reg)+"_", ""));
            irb.CreateStore(val, gep);
        };

        auto translateInstruction = [&irb, &getReg, &setReg, &pc, func, this](FrvInst* inst) -> void {
            llvm::Value* secondOp;
            llvm::Instruction::BinaryOps binOp;
            llvm::CmpInst::Predicate cmpPred;
            uint16_t loadStoreOpts; // bits for 8/16/32 (4,5,6) are set for the size of the load/store; bit number 2 is 1 for load, 0 for store; bit number 1 is 1 for signed, 0 for unsigned;
            enum{
                LD_ST_SIZE_8 = 8,
                LD_ST_SIZE_16 = 16,
                LD_ST_SIZE_32 = 32,
                LD_ST_SIGNED = 1 << 1,
                LD_ST_UNSIGNED = 0,
                LD_ST_LOAD = 1 << 0,
                LD_ST_STORE = 0,
            };
            const uint32_t LD_ST_SIZE_MASK = 0b111 << 3;
            switch(inst->mnem){
                case FRV_ADD:
                    binOp=llvm::Instruction::Add;      secondOp = getReg(inst->rs2);              goto translateBinOp;
                case FRV_SUB:
                    binOp=llvm::Instruction::Sub;      secondOp = getReg(inst->rs2);              goto translateBinOp;
                case FRV_SLL:
                    binOp=llvm::Instruction::Shl;      secondOp = getReg(inst->rs2 & 0x1f);       goto translateBinOp;
                case FRV_SRL:
                    binOp=llvm::Instruction::LShr;     secondOp = getReg(inst->rs2 & 0x1f);       goto translateBinOp;
                case FRV_SRA:
                    binOp=llvm::Instruction::AShr;     secondOp = getReg(inst->rs2 & 0x1f);       goto translateBinOp;
                case FRV_OR:
                    binOp=llvm::Instruction::Or;       secondOp = getReg(inst->rs2);              goto translateBinOp;
                case FRV_AND:
                    binOp=llvm::Instruction::And;      secondOp = getReg(inst->rs2);              goto translateBinOp;
                case FRV_XOR:
                    binOp=llvm::Instruction::Xor;      secondOp = getReg(inst->rs2);              goto translateBinOp;
                case FRV_SLT:
                    cmpPred = llvm::CmpInst::ICMP_SLT; secondOp = getReg(inst->rs2);              goto translateComparison;
                case FRV_SLTU:
                    cmpPred = llvm::CmpInst::ICMP_ULT; secondOp = getReg(inst->rs2);              goto translateComparison;

                case FRV_ADDI:
                    binOp=llvm::Instruction::Add;      secondOp = irb.getInt32(inst->imm);        goto translateBinOp;
                case FRV_SLLI:
                    binOp=llvm::Instruction::Shl;      secondOp = irb.getInt32(inst->imm & 0x1f); goto translateBinOp;
                case FRV_SRLI:
                    binOp=llvm::Instruction::LShr;     secondOp = irb.getInt32(inst->imm & 0x1f); goto translateBinOp;
                case FRV_SRAI:
                    binOp=llvm::Instruction::AShr;     secondOp = irb.getInt32(inst->imm & 0x1f); goto translateBinOp;
                case FRV_ORI:
                    binOp=llvm::Instruction::Or;       secondOp = irb.getInt32(inst->imm);        goto translateBinOp;
                case FRV_ANDI:
                    binOp=llvm::Instruction::And;      secondOp = irb.getInt32(inst->imm);        goto translateBinOp;
                case FRV_XORI:
                    binOp=llvm::Instruction::Xor;      secondOp = irb.getInt32(inst->imm);        goto translateBinOp;
                case FRV_SLTI:
                    cmpPred = llvm::CmpInst::ICMP_SLT; secondOp = irb.getInt32(inst->imm);        goto translateComparison;
                case FRV_SLTIU:
                    cmpPred = llvm::CmpInst::ICMP_ULT; secondOp = irb.getInt32(inst->imm);        goto translateComparison;

translateComparison:
                    setReg(inst->rd, irb.CreateIntCast(irb.CreateICmp(cmpPred, getReg(inst->rs1), secondOp), irb.getInt32Ty(), false));
                    break;

translateBinOp:
                    setReg(inst->rd, irb.CreateBinOp(binOp, getReg(inst->rs1), secondOp));
                    break;

                case FRV_LB: loadStoreOpts  = LD_ST_SIZE_8  | LD_ST_SIGNED   | LD_ST_LOAD;  goto translateLoadStore;
                case FRV_LH: loadStoreOpts  = LD_ST_SIZE_16 | LD_ST_SIGNED   | LD_ST_LOAD;  goto translateLoadStore;
                case FRV_LW: loadStoreOpts  = LD_ST_SIZE_32 | LD_ST_SIGNED   | LD_ST_LOAD;  goto translateLoadStore;
                case FRV_LBU: loadStoreOpts = LD_ST_SIZE_8  | LD_ST_UNSIGNED | LD_ST_LOAD;  goto translateLoadStore;
                case FRV_LHU: loadStoreOpts = LD_ST_SIZE_16 | LD_ST_UNSIGNED | LD_ST_LOAD;  goto translateLoadStore;

                case FRV_SB: loadStoreOpts = LD_ST_SIZE_8   | LD_ST_UNSIGNED | LD_ST_STORE; goto translateLoadStore;
                case FRV_SH: loadStoreOpts = LD_ST_SIZE_16  | LD_ST_UNSIGNED | LD_ST_STORE; goto translateLoadStore;
                case FRV_SW: loadStoreOpts = LD_ST_SIZE_32  | LD_ST_UNSIGNED | LD_ST_STORE; goto translateLoadStore;

translateLoadStore:
                    {
                    auto guestAddr = getReg(inst->rs1);
                    auto hostAddr = irb.CreateAdd(irb.CreateIntCast(guestAddr, irb.getInt64Ty(), false /* don't sign extend addresses */), irb.getInt64((uint64_t)guestMem));
                    auto loadStoreType = irb.getIntNTy(loadStoreOpts & LD_ST_SIZE_MASK);
                    if(loadStoreOpts & LD_ST_LOAD) {
                        auto load = irb.CreateLoad(loadStoreType, irb.CreateGEP(irb.getInt8Ty(),  irb.CreateIntToPtr(hostAddr, irb.getPtrTy()), {irb.getInt32(inst->imm)}));
                        setReg(inst->rd, irb.CreateIntCast(load, irb.getInt32Ty(), loadStoreOpts & LD_ST_SIGNED));
                    } else {
                        DEBUGLOG("storing to address " << guestMem << " plus register " << inst->rs1 << " plus immediate " << inst->imm);
                        // store
                        irb.CreateStore(
                            irb.CreateIntCast(getReg(inst->rs2), loadStoreType, true /* ignored because downcast */),
                            irb.CreateGEP(irb.getInt8Ty(), irb.CreateIntToPtr(hostAddr, irb.getPtrTy()), {irb.getInt32(inst->imm)})
                        );
                    }
                    }
                    break;

                case FRV_AUIPC:
                    {
                    auto immediateComputation = ((inst->imm) & 0xfffff000); // lower 12 bits are discarded
                    setReg(inst->rd, irb.CreateAdd(irb.getInt32(pc), irb.getInt32(immediateComputation)));
                    }
                    break;
                case FRV_LUI:
                    setReg(inst->rd, irb.getInt32(inst->imm & 0xfffff000)); // lower 12 bits are discarded
                    break;
                case FRV_ECALL:
                    {
                        auto* regs = func->arg_begin();
                        // this actually works lol. This is the sketchiest code I've ever written xD
                    irb.CreateCall(
                        llvm::FunctionCallee(llvm::FunctionType::get(irb.getVoidTy(), {irb.getPtrTy(), irb.getPtrTy()}, false),
                            hostPointerToLLVMPtr(irb, reinterpret_cast<void*>(syscall))),
                        {regs, hostPointerToLLVMPtr(irb, guestMem)}
                    );
                    }
                    //EXIT_TODO_X("ecall not implemented");
                    break;
                case FRV_FENCE:
                    EXIT_TODO_X("fence not implemented");
                    break;
                case FRV_FENCEI:
                    DEBUGLOG("encountered fence.i: no instruction cache, flushing nothing");
                    // TODO maybe yeet all translated code?
                    break;
                default:
                    char buf[4096] = {0};
                    frv_format(inst, sizeof(buf)-1, buf);
                    errx(2, "Unhandled instruction: %s\n", buf);
            }
        };

#ifndef NDEBUG
        uintptr_t originalPc = pc;
#endif

        // actual translation:
        // visit all but the last instruction
        for(unsigned i = 0; i < numInsts - 1; ++i){
            translateInstruction(&insts[i]);
            pc+=instructionSize;
        }

        assert(pc == originalPc + (numInsts-1)*instructionSize && "pc not updated correctly");
        // pc points to last instruction now

        if(foundJump){
            // last instruction is a jump.

            // 2 cases, conditional/unconditional:
            auto lastInst = &insts[numInsts-1];
            if(lastInst->mnem >= FRV_BEQ && lastInst->mnem <= FRV_BGEU){
                // conditional jump

                llvm::ICmpInst::Predicate cond;
                switch(lastInst->mnem){
                    case FRV_BEQ:  cond = llvm::ICmpInst::Predicate::ICMP_EQ;  break;
                    case FRV_BNE:  cond = llvm::ICmpInst::Predicate::ICMP_NE;  break;
                    case FRV_BLT:  cond = llvm::ICmpInst::Predicate::ICMP_SLT; break;
                    case FRV_BGE:  cond = llvm::ICmpInst::Predicate::ICMP_SGE; break;
                    case FRV_BLTU: cond = llvm::ICmpInst::Predicate::ICMP_ULT; break;
                    case FRV_BGEU: cond = llvm::ICmpInst::Predicate::ICMP_UGE; break;
                    default: errx(EXIT_FAILURE, "unhandled jump instruction, expected conditional jump");
                }
                auto cmp = irb.CreateICmp(cond, getReg(lastInst->rs1), getReg(lastInst->rs2));
                auto select = irb.CreateSelect(cmp, irb.getInt32(pc+lastInst->imm), irb.getInt32(pc+instructionSize));
                irb.CreateRet(select);
            }else{
                // unconditional jump
                assert((lastInst->mnem == FRV_JAL || lastInst->mnem == FRV_JALR) && "unhandled jump instruction, expected unconditional jump");

                // applies for both JAL and JALR
                setReg(lastInst->rd, irb.getInt32(pc+instructionSize)); // save next pc to rd: pc at start of chunk + num instructions * 4 (or pc now + 4)
                if(lastInst->mnem == FRV_JAL){ // JAL
                    irb.CreateRet(irb.getInt32(pc+lastInst->imm)); // return jump target
                }else{ // JALR
                    auto addr = irb.CreateAnd(irb.CreateAdd(getReg(lastInst->rs1), irb.getInt32(lastInst->imm)), 0xfffffffe);
                    irb.CreateRet(addr); // return jump target
                }
            }
        }else{
            // last instruction is not a jump, so just continue with next instruction
            translateInstruction(&insts[numInsts-1]);
            irb.CreateRet(irb.getInt32(pc+instructionSize)); // return next pc: pc at start of chunk + num instructions * 4 (or pc now + 4)
        }

        if constexpr (print){
            llvm::outs() << color::red << "Generated LLVM-IR:" << color::reset << "\n";
            func->print(llvm::outs(), nullptr);
        }
        return reinterpret_cast<ChunkFunc*>(compile(std::move(modUP), func->getName().str()));
    }

};

int main(int argc, char** argv) {
    // TODO actually make it commandline configurable
    auto args = ArgParse::parse(argc, argv);

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    unsigned commandlineConfigurableProgramLoadAddress = args.contains(ArgParse::possible.loadAddr) ? std::stoi(args[ArgParse::possible.loadAddr],0,16) : 0;
    DBT dbt{0x100000000, commandlineConfigurableProgramLoadAddress, args[ArgParse::possible.input]};

    bool step = args.contains(ArgParse::possible.step);;
    if(step)
        dbt.emulate<true>();
    else
        dbt.emulate<false>();

    return EXIT_SUCCESS;
}

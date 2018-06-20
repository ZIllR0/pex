/*
 * CapChecker
 * linux kernel capability checker
 * 2018 Tong Zhang<t.zhang2@partner.samsung.com>
 * TODO: remove critical functions which don't store
 */

#include "llvm-c/Core.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SetVector.h"

#include "cvfa.h"
//my aux headers
#include "commontypes.h"
#include "color.h"
#include "aux.h"
#include "stopwatch.h"
STOP_WATCH;

#if defined(DEBUG)
#undef DEBUG
#define DEBUG 0
#else
#define DEBUG 0
#endif

#define DEBUG_PREPARE 0
#define DEBUG_ANALYZE 1

using namespace llvm;

#define DEBUG_TYPE "capchk"


/*
 * define statistics if not enabled in LLVM
 */
#if defined(LLVM_ENABLE_STATS)
#undef LLVM_ENABLE_STATS
#endif

#if defined(NDEBUG)\
    || !defined(LLVM_ENABLE_STATS)\
    || (!LLVM_ENABLE_STATS)

#undef STATISTIC
#define CUSTOM_STATISTICS 1
#define STATISTIC(X,Y) \
    unsigned long X;\
const char* X##_desc = Y;

#define STATISTICS_DUMP(X) \
    errs()<<"    "<<X<<" : "<<X##_desc<<"\n";

#endif

STATISTIC(FuncCounter, "Functions greeted");
STATISTIC(ExternalFuncCounter, "External functions");
STATISTIC(DiscoveredPath, "Discovered Path");
STATISTIC(MatchedPath, "Matched Path");
STATISTIC(GoodPath, "Good Path");
STATISTIC(BadPath, "Bad Path");
STATISTIC(UnResolv, "Path Unable to Resolve");
STATISTIC(CSFPResolved, "Resolved CallSite Using Function Pointer");
STATISTIC(CRITVAR, "Critical Variables");
STATISTIC(CRITFUNC, "Critical Functions");
STATISTIC(FwdAnalysisMaxHit, "# of times max depth for forward analysis hit");
STATISTIC(BwdAnalysisMaxHit, "# of times max depth for backward analysis hit");
STATISTIC(CPUnResolv, "Critical Function Pointer Unable to Resolve");
STATISTIC(CPResolv, "Critical Function Pointer Resolved");
STATISTIC(CFuncUsedByNonCall, "Critical Functions used by non CallInst");
STATISTIC(MatchCallCriticalFuncPtr, "# of times indirect call site matched with critical functions");
STATISTIC(UnMatchCallCriticalFuncPtr, "# of times indirect call site failed to match with critical functions");
STATISTIC(CapChkInFPTR, "found capability check inside call using function ptr\n");

////////////////////////////////////////////////////////////////////////////////
//map function to its check instruction
Function2ChkInst f2ci;
Value2ChkInst v2ci;

//t2fs is used to fuzzy matching calling using function pointer
TypeToFunctions t2fs;

//map function to check instructions inside that function
Function2ChkInst f2chks;

//all function pointer assignment,(part of function use)
InstructionSet fptrassign;
//stores all indirect call sites
InDirectCallSites idcs;
//store indirect call site to its candidates
ConstInst2Func idcs2callee;

ValueList critical_variables;

//all functions in the kernel
FunctionSet all_functions;

#define MAX_PATH 1000
#define MAX_BACKWD_SLICE_DEPTH 100
#define MAX_FWD_SLICE_DEPTH 100

/*
 * for stdlib
 */
bool cmp_llvm_val(const Value* a, const Value* b)
{
    unsigned long _a = (unsigned long)a;
    unsigned long _b = (unsigned long)b;
    return a>b;
}

class capchk : public ModulePass
{
    private:
        bool runOnModule(Module &);
        bool capchkPass(Module &);

        //capability checker
        void process_intras(Module& module);
        void process_cpgf(Module& module);

        /*
         * prepare
         */
        void collect_kernel_init_functions(Module& module);
        void collect_wrappers(Module& module);
        void collect_crits(Module& module);
        void collect_pp(Module& module);
        void collect_chkps(Module&);
        void resolve_all_indirect_callee(Module& module);

        void check_critical_function_usage(Module& module);
        void check_critical_variable_usage(Module& module);

        void forward_all_interesting_usage(Instruction* I, int depth,
                bool checked, InstructionList &callgraph,
                InstructionList& chks);
        
        /*
         * analyze
         */
        _REACHABLE backward_slice_build_callgraph(InstructionList &callgraph,
                Instruction* I, FunctionToCheckResult& fvisited);
        _REACHABLE _backward_slice_reachable_to_chk_function(Instruction* I);
        void backward_slice_reachable_to_chk_function(Instruction* I);

        _REACHABLE bs_using_indcs(Function*,
                InstructionList& callgraph, FunctionToCheckResult& visited);
        _REACHABLE match_cs_using_fptr_method_0(Function*,
                InstructionList& callgraph, FunctionToCheckResult& visited);
        _REACHABLE match_cs_using_fptr_method_1(Function*,
                InstructionList& callgraph, FunctionToCheckResult& visited);
        _REACHABLE match_cs_using_fptr_method_2(Function*,
                InstructionList& callgraph, FunctionToCheckResult& visited);

        FunctionSet resolve_indirect_callee(CallInst*);


#ifdef CUSTOM_STATISTICS
        void dump_statistics();
#endif

        //Other checker...
        //used by chk_unsafe_access
        bool is_safe_access(Instruction *ins, Value* addr, uint64_t type_size);
        void chk_div0(Module& module);
        void chk_unsafe_access(Module& module);

        /*
         * several aux helper functions
         */
        bool is_complex_type(Type*);
        bool is_rw_global(Value*);
        Value* get_global_def(Value*);
        Value* get_global_def(Value*, ValueSet&);
        bool is_kernel_init_functions(Function* f);
        bool is_kernel_init_functions(Function* f, FunctionSet& visited);

        int use_parent_func_arg(Value*, Function*);


        /*
         * context for current module
         */
        LLVMContext *ctx;
        Module* m;
        /*
         * for debug purpose
         */
        InstructionList dbgstk;
        void dump_dbgstk();
        void dump_callstack(InstructionList& callstk);

        void dump_chk_and_wrap();
        void dump_f2ci();
        void dump_v2ci();
        void dump_kinit();
        void dump_non_kinit();

    public:
        static char ID;
        capchk() : ModulePass(ID){};

        const char* getPassName()
        {
            return "capchk";
        }
        void getAnalysisUsage(AnalysisUsage &au) const override
        {
            //au.addRequired<AAResultsWrapperPass>();
            //au.addRequired<TargetLibraryInfoWrapperPass>();
            //au.addRequired<ScalarEvolutionWrapperPass>();
            au.setPreservesAll();
        }
};

#ifdef CUSTOM_STATISTICS
void capchk::dump_statistics()
{
    errs()<<"------------STATISTICS---------------\n";
    STATISTICS_DUMP(FuncCounter);
    STATISTICS_DUMP(ExternalFuncCounter);
    STATISTICS_DUMP(DiscoveredPath);
    STATISTICS_DUMP(MatchedPath);
    STATISTICS_DUMP(GoodPath);
    STATISTICS_DUMP(BadPath);
    STATISTICS_DUMP(UnResolv);
    STATISTICS_DUMP(CSFPResolved);
    STATISTICS_DUMP(CRITFUNC);
    STATISTICS_DUMP(CRITVAR);
    STATISTICS_DUMP(FwdAnalysisMaxHit);
    STATISTICS_DUMP(BwdAnalysisMaxHit);
    STATISTICS_DUMP(CPUnResolv);
    STATISTICS_DUMP(CPResolv);
    STATISTICS_DUMP(CFuncUsedByNonCall);
    STATISTICS_DUMP(MatchCallCriticalFuncPtr);
    STATISTICS_DUMP(UnMatchCallCriticalFuncPtr);
    STATISTICS_DUMP(CapChkInFPTR);
    errs()<<"\n\n\n";
}
#endif

char capchk::ID;

/*
 * command line options
 */
cl::opt<bool> knob_capchk_critical_var("ccv",
        cl::desc("check critical variable usage - disabled by default"),
        cl::init(false));

cl::opt<bool> knob_capchk_critical_fun("ccf",
        cl::desc("check critical function usage - enabled by default"),
        cl::init(true));

cl::opt<bool> knob_capchk_ccfv("ccfv",
        cl::desc("print path to critical function(collect phase) - disabled by default"),
        cl::init(false));

cl::opt<bool> knob_capchk_ccvv("ccvv",
        cl::desc("print path to critical variable(collect phase) - disabled by default"),
        cl::init(false));

cl::opt<bool> knob_capchk_f2c("f2c",
        cl::desc("print critical function to capability mapping - enabled by default"),
        cl::init(true));

cl::opt<bool> knob_capchk_v2c("v2c",
        cl::desc("print critical variable to capability mapping - enabled by default"),
        cl::init(true));

cl::opt<bool> knob_capchk_caw("caw",
        cl::desc("print check functions and wrappers discovered - enabled by default"),
        cl::init(true));

cl::opt<bool> knob_capchk_kinit("kinit",
        cl::desc("print kernel init functions - enabled by default"),
        cl::init(true));

cl::opt<bool> knob_capchk_nkinit("nkinit",
        cl::desc("print kernel non init functions - enabled by default"),
        cl::init(true));

cl::opt<bool> knob_capchk_cvf("cvf",
        cl::desc("complex value flow analysis - disabled by default"),
        cl::init(false));


/*
 * helper function
 */
Instruction* GetNextInstruction(Instruction* I)
{
    if (isa<TerminatorInst>(I))
        return I;
    BasicBlock::iterator BBI(I);
    return dyn_cast<Instruction>(++BBI);
}

Instruction* GetNextNonPHIInstruction(Instruction* I)
{
    if (isa<TerminatorInst>(I))
        return I;
    BasicBlock::iterator BBI(I);
    while(isa<PHINode>(BBI))
        ++BBI;
    return dyn_cast<Instruction>(BBI);
}

/*
 * all critical functions
 * should be permission checked before use
 * generate critical functions on-the-fly
 */
FunctionSet critical_functions;

bool is_critical_function(Function* f)
{
    return critical_functions.count(f)!=0;
}

/*
 * permission check functions,
 * those function are used to perform permission check before
 * using critical resources
 */
static const char* check_functions [] = 
{
    "capable",
    "ns_capable",
};

bool is_check_function(const std::string& str)
{
    if (std::find(std::begin(check_functions),
                std::end(check_functions),
                str) != std::end(check_functions))
    {
        return true;
    }
    return false;                                  
}

/*
 * skip those variables
 */
static const char* skip_var [] = 
{
    "jiffies",
    "nr_cpu_ids",
    "nr_irqs",
    "nr_threads",
};

bool is_skip_var(const std::string& str)
{
    return std::find(std::begin(skip_var), std::end(skip_var), str)
            != std::end(skip_var);
}

StringRef get_callee_function_name(Instruction* i)
{
    CallInst* ci = dyn_cast<CallInst>(i);
    if (Function* f = ci->getCalledFunction())
    {
        return f->getName();
    }
    Value* cv = ci->getCalledValue();
    Function* f = dyn_cast<Function>(cv->stripPointerCasts());
    if (f)
    {
        return f->getName();
    }
    return "";
}

/*
 * common interface which is not considered dangerous function
 * some of those functions need to analyze together with parameters
 * (require SVF, data flow analysis)
 */
static const char* skip_functions [] = 
{
    //may operate on wrong source?
    "add_taint",
    "__mutex_init",
    "mutex_lock",
    "mutex_unlock",
    "schedule",
    "_cond_resched",
    "printk",
    "__kmalloc",
    "_copy_to_user",
    "_do_fork",
    "__memcpy",
    "strncmp",
    "strlen",
    "strim",
    "strchr",
    "strcmp",
    "strcpy",
    "strncat",
    "strlcpy",
    "strscpy",
    "strsep",
    "strndup_user",
    "strnlen_user",
    "sscanf",
    "snprintf",
    "scnprintf",
    "sort",
    "prandom_u32",
    "memchr",
    "memcmp",
    "memset",
    "memmove",
    "skip_spaces",
    "kfree",
    "kmalloc",
    "kstrdup",
    "kstrtoull",
    "kstrtouint",
    "kstrtoint",
    "kstrtobool",
    "strncpy_from_user",
    "kstrtoul_from_user",
    "__msecs_to_jiffies",
    "drm_printk",
    "cpumask_next_and",
    "cpumask_next",
    "dump_stack",//break KASLR here?
    "___ratelimit",
    "simple_strtoull",
    "simple_strtoul",
    "dec_ucount",
    "inc_ucount",
    "jiffies_to_msecs",
    "__warn_printk",//break KASLR here?
    "arch_release_task_struct",
    "do_syscall_64",//syscall entry point
    "do_fast_syscall_32",
    "do_int80_syscall_32",
    "complete",
    "__wake_up",
    "mutex_trylock",
    "finish_wait",
    "__init_waitqueue_head",
    "complete",
    "mutex_lock_interruptible",
    "up_write",
    "up_read",
    "down_write_trylock",
    "down_write",
    "down_read",
    "find_vma",
    "vzalloc",
    "vmalloc",
    "vfree",
    "vmalloc_to_page",
    "__vmalloc",
    "kfree_call_rcu",
    "kvfree",
    "krealloc",
    "_copy_from_user",
    "__free_pages",
    "__put_page",
    "kvmalloc_node",
    "free_percpu",
    "__alloc_percpu",
    "get_user_pages",
    "__mm_populate",
    "dput",
    "d_path",
    "iput",
    "inode_dio_wait",
    "current_time",
    "is_bad_inode",
    "__fdget",
    "mntput",
    "mntget",
    "seq_puts",
    "seq_putc",
    "seq_printf",
    "blkdev_put",
    "blkdev_get",
    "bdget",
    "bdput",
    "bdgrab",
    "thaw_bdev",
    "__brelse",
    "nla_parse",
    "dev_warn",
    "dev_printk",
    "dev_notice",
    "dev_alert",
};

bool is_skip_function(const std::string& str)
{
    return std::find(std::begin(skip_functions), std::end(skip_functions), str)
            != std::end(skip_functions);
}

/*
 * file/dev op handler and sys call prefix/suffix
 */
static const char* interesting_keyword [] = 
{
    "SyS",
    "sys",
    "open",
    "release",
    "lseek",
    "read",
    "write",
    "sync",
    "ioctl",
};

bool contains_interesting_kwd(const std::string& str)
{
    for (auto i = std::begin(interesting_keyword);
            i!=std::end(interesting_keyword); ++i)
    {
        std::size_t found = str.find(*i);
        if (found != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

/*
 * interesting type which contains functions pointers to deal with user request
 */
static const char* interesting_type_word [] = 
{
    "file_operations",
};

static const char* kernel_start_functions [] = 
{
    "start_kernel",
    "x86_64_start_kernel",
};

StringList kernel_init_functions;
StringSet non_kernel_init_functions;

/*
 * record capability parameter position passed to capability check function
 * all discovered wrapper function to check functions will also have one entry
 */
FunctionData chk_func_cap_position;

bool is_function_chk_or_wrapper(Function* f)
{
    return chk_func_cap_position.find(f)!=chk_func_cap_position.end();
}

////////////////////////////////////////////////////////////////////////////////
/*
 * debug function, track process progress internally
 */
void capchk::dump_dbgstk()
{
    errs()<<ANSI_COLOR_GREEN<<"Process Stack:"<<ANSI_COLOR_RESET<<"\n";
    int cnt = 0;

    for (auto* I: dbgstk)
    {
        errs()<<""<<cnt<<" "<<I->getFunction()->getName()<<" ";
        I->getDebugLoc().print(errs());
        errs()<<"\n";
        cnt++;
    }
    errs()<<ANSI_COLOR_GREEN<<"-------------"<<ANSI_COLOR_RESET<<"\n";
}

void capchk::dump_callstack(InstructionList& callstk)
{
    errs()<<ANSI_COLOR_GREEN<<"Call Stack:"<<ANSI_COLOR_RESET<<"\n";
    int cnt = 0;

    for (auto* I: callstk)
    {
        errs()<<""<<cnt<<" "<<I->getFunction()->getName()<<" ";
        I->getDebugLoc().print(errs());
        errs()<<"\n";
        cnt++;
    }
    errs()<<ANSI_COLOR_GREEN<<"-------------"<<ANSI_COLOR_RESET<<"\n";
}

void capchk::dump_v2ci()
{
    if (!knob_capchk_v2c)
        return;
    errs()<<ANSI_COLOR(BG_BLUE,FG_WHITE)
        <<"--- Variables Protected By Capability---"
        <<ANSI_COLOR_RESET<<"\n";
    for (auto& cis: v2ci)
    {
        Value* v = cis.first;
        errs()<<ANSI_COLOR_GREEN<<v->getName()<<ANSI_COLOR_RESET<<"\n";
        for (auto *ci: *cis.second)
        {
            CallInst* cs = dyn_cast<CallInst>(ci);
            Function* cf = cs->getCalledFunction();
            int cap_no = -1;
            if (is_function_chk_or_wrapper(cf))
            {
                cap_no = chk_func_cap_position[cf];
                Value* capv = cs->getArgOperand(cap_no);
                if (!isa<ConstantInt>(capv))
                {
                    cs->getDebugLoc().print(errs());
                    errs()<<"\n";
                    cs->print(errs());
                    errs()<<"\n";
                    //llvm_unreachable("expect ConstantInt in capable");
                    errs()<<"Dynamic Load CAP\n";
                    cs->getDebugLoc().print(errs());
                    errs()<<"\n";
                    continue;
                }
                cap_no = dyn_cast<ConstantInt>(capv)->getSExtValue();
            }
            assert((cap_no>=CAP_CHOWN) && (cap_no<=CAP_LAST_CAP));
            errs()<<"    "<<cap2string[cap_no]<<" @ ";
            cs->getDebugLoc().print(errs());
            errs()<<"\n";
        }
    }
}

void capchk::dump_f2ci()
{
    if (!knob_capchk_f2c)
        return;
    errs()<<ANSI_COLOR(BG_BLUE,FG_WHITE)
        <<"--- Function Protected By Capability---"
        <<ANSI_COLOR_RESET<<"\n";
    for (auto& cis: f2ci)
    {
        Function* func = cis.first;
        errs()<<ANSI_COLOR_GREEN<<func->getName()<<ANSI_COLOR_RESET<<"\n";
        for (auto *ci: *cis.second)
        {
            CallInst* cs = dyn_cast<CallInst>(ci);
            Function* cf = cs->getCalledFunction();
            int cap_no = -1;
            if (is_function_chk_or_wrapper(cf))
            {
                cap_no = chk_func_cap_position[cf];
                Value* capv = cs->getArgOperand(cap_no);
                if (!isa<ConstantInt>(capv))
                {
                    cs->getDebugLoc().print(errs());
                    errs()<<"\n";
                    cs->print(errs());
                    errs()<<"\n";
                    //llvm_unreachable("expect ConstantInt in capable");
                    errs()<<"Dynamic Load CAP\n";
                    cs->getDebugLoc().print(errs());
                    errs()<<"\n";
                    continue;
                }
                cap_no = dyn_cast<ConstantInt>(capv)->getSExtValue();
            }
            assert((cap_no>=CAP_CHOWN) && (cap_no<=CAP_LAST_CAP));
            errs()<<"    "<<cap2string[cap_no]<<" @ ";
            cs->getDebugLoc().print(errs());
            errs()<<"\n";
        }
    }
}

void capchk::dump_kinit()
{
    if (!knob_capchk_kinit)
        return;
    errs()<<ANSI_COLOR(BG_BLUE, FG_WHITE)
            <<"=Kernel Init Functions="
            <<ANSI_COLOR_RESET<<"\n";
    for (auto I: kernel_init_functions)
    {
        errs()<<I<<"\n";
    }
    errs()<<"=o=\n";
}

void capchk::dump_non_kinit()
{
    if (!knob_capchk_nkinit)
        return;
    errs()<<ANSI_COLOR(BG_BLUE, FG_WHITE)
            <<"=NON-Kernel Init Functions="
            <<ANSI_COLOR_RESET<<"\n";
    for (auto I: non_kernel_init_functions)
    {
        errs()<<I<<"\n";
    }
    errs()<<"=o=\n";
}

void capchk::dump_chk_and_wrap()
{
    if (!knob_capchk_caw)
        return;
    errs()<<ANSI_COLOR(BG_BLUE, FG_WHITE)
        <<"=chk functions and wrappers="
        <<ANSI_COLOR_RESET<<"\n";
    for (auto &f2p: chk_func_cap_position)
    {
        errs()<<". "<<f2p.first->getName()
            <<"  @ "<<f2p.second
            <<"\n";
    }
    errs()<<"=o=\n";
}

////////////////////////////////////////////////////////////////////////////////

bool capchk::is_safe_access(Instruction* ins,Value* addr, uint64_t type_size)
{
    uint64_t size;
    uint64_t offset;
    bool result;

    bool know = false;
    std::string reason = "";

    if(isa<GlobalVariable>(addr))
    {
        GlobalVariable* gv = dyn_cast<GlobalVariable>(addr);
        if(gv->getLinkage()==GlobalValue::ExternalLinkage)
        {
            goto fallthrough;
        }
        if (!gv->hasInitializer())
        {
            //we have no idea???
            goto fallthrough;
        }
        Constant* initializer = gv->getInitializer();
        Type* itype = initializer->getType();
        unsigned allocated_size = m->getDataLayout()
            .getTypeAllocSize(itype);
        size = allocated_size;
        offset = 0;
    }else
    {
fallthrough:
        const TargetLibraryInfo * TLI = 
            &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
        const DataLayout &dl = m->getDataLayout();
        ObjectSizeOffsetVisitor* obj_size_vis;
        ObjectSizeOpts ObjSizeOptions;
        ObjSizeOptions.RoundToAlign = true;
        obj_size_vis = new ObjectSizeOffsetVisitor(dl, TLI, *ctx, ObjSizeOptions);

        SizeOffsetType size_offset = obj_size_vis->compute(addr);
        if (!obj_size_vis->bothKnown(size_offset))
        {
            if (obj_size_vis->knownSize(size_offset))
            {
                reason += "size: " + size_offset.first.getZExtValue();
            }else
            {
                reason += "size: NA ";
            }
            reason += " ";
            if (obj_size_vis->knownOffset(size_offset))
            {
                reason += "Offset: " + size_offset.second.getSExtValue();
            }else
            {
                reason += "offset: NA";
            }
            result = false;
            goto dead_or_alive;
        }
        know = true;
        size = size_offset.first.getZExtValue();
        offset = size_offset.second.getSExtValue();
    }
    result = (offset >= 0) && (size >= uint64_t(offset)) &&
        ((size - uint64_t(offset)) >= (type_size / 8));

dead_or_alive:
    if (know)
    {   
        if (!result)
        {
            errs()<<" "<<ins->getParent()->getParent()->getName()<<":"<<ANSI_COLOR_RED;
            ins->getDebugLoc().print(errs());
            errs()<<ANSI_COLOR_RESET"\n";

            errs()<<ANSI_COLOR_RED
                <<"NOT SAFE, "
                <<ANSI_COLOR_RESET
                <<"Reason:"
                <<reason<<"  "
                <<"Offset:"<<offset
                <<", size:"<<size<<"\n";
        }
    }else//unknown
    {
        errs()<<" unknown "<<ins->getParent()->getParent()->getName()
            <<":"<<ANSI_COLOR_RED;
        ins->getDebugLoc().print(errs());
        errs()<<ANSI_COLOR_RESET"\n";

        errs()<<"Reason:"<<reason<<"\n";
    }
    return result;
}

void capchk::chk_unsafe_access(Module& module)
{
    size_t total_dereference = 0;
    std::map<Value*, Value*> safe_access_list;

    for (Module::iterator mi = module.begin(), me = module.end();
            mi != me; ++mi)
    {
        Function *func = dyn_cast<Function>(mi);
        if (func->isDeclaration())
        {
            continue;
        }
        for(Function::iterator fi = func->begin(), fe = func->end();
                fi != fe; ++fi)
        {
            BasicBlock* blk = dyn_cast<BasicBlock>(fi);
            for (BasicBlock::iterator bi = blk->begin(), be = blk->end();
                    bi != be; ++bi)
            {
                Value* ptr_operand;
                uint64_t rwsize;
                if(isa<LoadInst>(bi))
                {
                    LoadInst* load = dyn_cast<LoadInst>(bi);
                    ptr_operand = load->getPointerOperand();
                    rwsize = module.getDataLayout()
                        .getTypeStoreSizeInBits(load->getType());
                }else if(isa<StoreInst>(bi))
                {
                    StoreInst* store = dyn_cast<StoreInst>(bi);
                    ptr_operand = store->getPointerOperand();
                    rwsize = module.getDataLayout()
                        .getTypeStoreSizeInBits(store
                                ->getValueOperand()
                                ->getType());
                }else
                {
                    continue;
                }
                total_dereference++;
                if (is_safe_access(dyn_cast<Instruction>(bi),ptr_operand, rwsize))
                {
                    safe_access_list[dyn_cast<Instruction>(bi)] = ptr_operand;
                }
            }
        }
    }
    errs()<<" "
        <<safe_access_list.size()
        <<"/"
        <<total_dereference
        <<" safe access collected\n";
}
////////////////////////////////////////////////////////////////////////////////
//DIV 0 checker
static bool mayDivideByZero(Instruction *I) {
    if (!(I->getOpcode() == Instruction::UDiv ||
                I->getOpcode() == Instruction::SDiv ||
                I->getOpcode() == Instruction::URem ||
                I->getOpcode() == Instruction::SRem))
    {
        return false;
    }
    Value *Divisor = I->getOperand(1);
    auto *CInt = dyn_cast<ConstantInt>(Divisor);
    return !CInt || CInt->isZero();
}

void capchk::chk_div0(Module& module)
{
    for (Module::iterator f_begin = module.begin(), f_end = module.end();
            f_begin != f_end; ++f_begin)
    {
        Function *func = dyn_cast<Function>(f_begin);
        if (func->isDeclaration())
            continue;
        //errs()<<func->getName()<<"\n";
        std::set<BasicBlock*> bb_visited;
        std::queue<BasicBlock*> bb_work_list;
        bb_work_list.push(&func->getEntryBlock());
        while(bb_work_list.size())
        {
            BasicBlock* bb = bb_work_list.front();
            bb_work_list.pop();
            if(bb_visited.count(bb))
            {
                continue;
            }
            bb_visited.insert(bb);

            for (BasicBlock::iterator ii = bb->begin(),
                    ie = bb->end();
                    ii!=ie; ++ii)
            {
                Instruction* pvi = dyn_cast<Instruction>(ii);
                if (!mayDivideByZero(pvi))
                {
                    continue;
                }
                errs()<<" "<<func->getName()<<":"<<ANSI_COLOR_RED;
                pvi->getDebugLoc().print(errs());
                errs()<<ANSI_COLOR_RESET"\n";
                pvi->print(errs());
                errs()<<"\n";
            }
            /*
             * insert all successor of current basic block to work list
             */
            for (succ_iterator si = succ_begin(bb), se = succ_end(bb); si!=se; ++si)
                bb_work_list.push(cast<BasicBlock>(*si));
        }
    }
}
////////////////////////////////////////////////////////////////////////////////

/*
 * is this function type contains non-trivial(non-primary) type?
 */
bool capchk::is_complex_type(Type* t)
{
    if (!t->isFunctionTy())
        return false;
    if (t->isFunctionVarArg())
        return true;
    FunctionType *ft = dyn_cast<FunctionType>(t);
    //params
    int number_of_complex_type = 0;
    for (int i = 0; i<ft->getNumParams(); i++)
    {
        Type* argt = ft->getParamType(i);
strip_pointer:
        if (argt->isPointerTy())
        {
            argt = argt->getPointerElementType();
            goto strip_pointer;
        }

        if (argt->isSingleValueType())
            continue;
        number_of_complex_type++;
    }
    //return type
    Type* rt = ft->getReturnType();

again://to strip pointer
    if (rt->isPointerTy())
    {
        Type* pet = rt->getPointerElementType();
        if (pet->isPointerTy())
        {
            rt = pet;
            goto again;
        }
        if (!pet->isSingleValueType())
        {
            number_of_complex_type++;
        }
    }

    return (number_of_complex_type!=0);
}

/*
 * def/use global?
 * take care of phi node using `visited'
 */
Value* capchk::get_global_def(Value* val, ValueSet& visited)
{
    if (visited.count(val)!=0)
        return NULL;
    visited.insert(val);
    if (isa<GlobalValue>(val))
        return val;
    Instruction* vali = dyn_cast<Instruction>(val);
    if (!vali)
        return NULL;
    for (auto *U : vali->users())
    {
        Value* v = get_global_def(U, visited);
        if (v)
            return v;
    }
    return NULL;
}

Value* capchk::get_global_def(Value* val)
{
    ValueSet visited;
    return get_global_def(val, visited);
}

bool capchk::is_rw_global(Value* val)
{
    ValueSet visited;
    return get_global_def(val, visited)!=NULL;
}


/*
 * user can trace back to function argument?
 * only support simple wrapper
 * return the cap parameter position in parameter list
 * return -1 for not found
 */
int capchk::use_parent_func_arg(Value* v, Function* f)
{
    int cnt = 0;
    for (auto a = f->arg_begin(), b = f->arg_end(); a!=b; ++a)
    {
        if (dyn_cast<Value>(a)==v)
            return cnt;
        cnt++;
    }
    return -1;
}
/*
 * is this functions part of the kernel init sequence?
 * if function f has single user which goes to start_kernel(),
 * then this is a init function
 */
bool capchk::is_kernel_init_functions(Function* f, FunctionSet& visited)
{
    std::string name = f->getName();
    if (std::find(std::begin(kernel_init_functions),
                std::end(kernel_init_functions),
                name) != std::end(kernel_init_functions))
        return true;
    if (non_kernel_init_functions.count(name)!=0)
        return false;

    //init functions with initcall prefix belongs to kernel init sequence
    std::string std_gv_spec = "__initcall_" + name;
    for (GlobalVariable &gvi: m->globals())
    {
        GlobalValue* gi = &gvi;
        if (Value* gv = dyn_cast<Value>(gi))
        {
            if (gv->getName().startswith(std_gv_spec))
            {
                kernel_init_functions.push_back(name);
                return true;
            }
        }
    }

    //not found in cache?
    //all path that can reach to f should start from start_kernel()
    //look backward(find who used f)
    FunctionList flist;
    for (auto *U : f->users())
        if (CallInst* cs = dyn_cast<CallInst>(U))
            flist.push_back(cs->getFunction());

    //no user?
    if (flist.size()==0)
    {
        non_kernel_init_functions.insert(name);
        return false;
    }

    visited.insert(f);
    while (flist.size())
    {
        Function* xf = flist.front();
        flist.pop_front();
        if (visited.count(xf))
            continue;
        visited.insert(xf);
        if (!is_kernel_init_functions(xf, visited))
        {
            non_kernel_init_functions.insert(name);
            return false;
        }
    }
    kernel_init_functions.push_back(name);
    return true;
}

bool capchk::is_kernel_init_functions(Function* f)
{
    FunctionSet visited;
    return is_kernel_init_functions(f, visited);
}

FunctionSet capchk::resolve_indirect_callee(CallInst* ci)
{
    FunctionSet fs;
    Function* callee = NULL;
    if (ci->isInlineAsm())
        return fs;
    if (callee = ci->getCalledFunction())
    {
        //not indirect call
        fs.insert(callee);
        return fs;
    }
    Value* cv = ci->getCalledValue();
    callee = dyn_cast<Function>(cv->stripPointerCasts());
    if (callee)
    {
        fs.insert(callee);
        return fs;
    }
    //FUZZY MATCHING
    //method 1: signature based matching
    //only allow precise match when collecting protected functions
    if (!knob_capchk_cvf)
    {
        Type *ft = cv->getType()->getPointerElementType();
        if (!is_complex_type(ft))
        {
            return fs;
        }
        std::set<Function*> *fl = t2fs[ft];
        if (fl==NULL)
            return fs;
        for (auto* f: *fl)
        {
            fs.insert(f);
        }
    }else
    {
    //method 2: use svf to figure out
        if (FunctionSet* _fs = idcs2callee[ci])
            for (auto* f: *_fs)
                fs.insert(f);
    }
    return fs;
}

void capchk::collect_kernel_init_functions(Module& module)
{
    Function *kstart = NULL;
    FunctionSet kif;
    kernel_init_functions.push_back("x86_64_start_kernel");
    kernel_init_functions.push_back("start_kernel");
    for (Module::iterator fi = module.begin(), f_end = module.end();
            fi != f_end; ++fi)
    {
        Function *func = dyn_cast<Function>(fi);
        if (func->isDeclaration() || func->isIntrinsic())
            continue;
        if (func->hasName())
        {
            StringRef fname = func->getName();
            if (fname.startswith("x86_64_start_kernel"))
            {
                errs()<<ANSI_COLOR_GREEN
                    <<"Found "<<func->getName()
                    <<ANSI_COLOR_RESET<<"\n";
                kstart = func;
                break;
            }else if (fname.startswith("start_kernel"))
            {
                //we should consider start_kernel as kernel init functions no
                //matter what
                kif.insert(func);
                for (auto *U: func->users())
                {
                    if (Instruction *I = dyn_cast<Instruction>(U))
                    {
                        kif.insert(I->getFunction());
                    }
                }
            }
        }
    }
    assert(kstart!=NULL);
    kif.insert(kstart);

    //find all init functions starting from x86_64_start_kernel
    FunctionSet func_visited;
    FunctionList func_work_list;
    func_work_list.push_back(kstart);

    while (func_work_list.size())
    {
        Function* cfunc = func_work_list.front();
        func_work_list.pop_front();

        if (cfunc->isDeclaration() || cfunc->isIntrinsic())
            continue;
        
        kif.insert(cfunc);
        func_visited.insert(cfunc);
        kernel_init_functions.push_back(cfunc->getName());

        //for current function
        for(Function::iterator fi = cfunc->begin(), fe = cfunc->end(); fi != fe; ++fi)
        {
            BasicBlock* bb = dyn_cast<BasicBlock>(fi);
            for (BasicBlock::iterator ii = bb->begin(), ie = bb->end(); ii!=ie; ++ii)
            {
                CallInst* ci = dyn_cast<CallInst>(ii);
                if (!ci)
                {
                    continue;
                }
                if (Function* nf = ci->getCalledFunction())
                {
                    if (nf->isDeclaration() || 
                        nf->isIntrinsic() ||
                        func_visited.count(nf))
                        continue;
                    func_work_list.push_back(nf);
                }else if (Value* nv = ci->getCalledValue())
                {
                    //function pointer?
                    FunctionSet fs = resolve_indirect_callee(ci);
                    for (auto callee: fs)
                        if (!func_visited.count(callee))
                            func_work_list.push_back(callee);
                }
            }
        }
    }
    kernel_init_functions.sort();
    kernel_init_functions.erase(
            std::unique(kernel_init_functions.begin(), kernel_init_functions.end()),
            kernel_init_functions.end());

    /*
     * ! BUG: query use of inlined function result in in-accurate result?
     * inlined foo();
     * bar(){zoo()} zoo(){foo};
     *
     * query user{foo()} -> zoo()
     * query BasicBlocks in bar -> got call instruction in bar()?
     */
    //remove all non_kernel_init_functions from kernel_init_functions
    //purge all over approximation
    int last_count = 0;

again:
    for (auto I: kif)
    {
        if ((I->getName()=="start_kernel") ||
            (I->getName()=="x86_64_start_kernel"))
        {
            continue;
        }
        for (auto *U: I->users())
        {
            if (!isa<Instruction>(U))
                continue;
            if (kif.count(dyn_cast<Instruction>(U)->getFunction())==0)
            {
                //means that we have a user does not belong to kernel init functions
                //we need to remove it
                non_kernel_init_functions.insert(I->getName());
                break;
            }
        }
    }

    for (auto I: non_kernel_init_functions)
    {
        auto i = std::find(kernel_init_functions.begin(),
                    kernel_init_functions.end(),I);
        if (i!=kernel_init_functions.end())
            kernel_init_functions.erase(i);
    }
    if (last_count!=non_kernel_init_functions.size())
    {
        last_count = non_kernel_init_functions.size();
        goto again;
    }
#if 1
//this is imprecise, clear it
    errs()<<"clear NON-kernel-init functions\n";
    non_kernel_init_functions.clear();
#endif
    dump_kinit();
}

void capchk::collect_chkps(Module& module)
{
    for (Module::iterator fi = module.begin(), f_end = module.end();
            fi != f_end; ++fi)
    {
        Function *func = dyn_cast<Function>(fi);
        if (func->isDeclaration() || func->isIntrinsic()
                ||is_function_chk_or_wrapper(func))
            continue;
        
        InstructionSet *chks = f2chks[func];
        if (!chks)
        {
            chks = new InstructionSet();
            f2chks[func] = chks;
        }

        for(Function::iterator fi = func->begin(), fe = func->end(); fi != fe; ++fi)
        {
            BasicBlock* bb = dyn_cast<BasicBlock>(fi);
            for (BasicBlock::iterator ii = bb->begin(), ie = bb->end(); ii!=ie; ++ii)
            {
                CallInst* ci = dyn_cast<CallInst>(ii);
                if (!ci)
                    continue;
                if (Function* _f = ci->getCalledFunction())
                {
                    if (is_function_chk_or_wrapper(_f))
                        chks->insert(ci);
                    continue;
                }
                Value* cv = ci->getCalledValue();
                Function* _cf = dyn_cast<Function>(cv->stripPointerCasts());
                if(_cf && is_function_chk_or_wrapper(_cf))
                    chks->insert(ci);
            }
        }
    }
}

void capchk::collect_wrappers(Module& module)
{
    //add capable and ns_capable
    for (Module::iterator fi = module.begin(), f_end = module.end();
            fi != f_end; ++fi)
    {
        Function *func = dyn_cast<Function>(fi);
        StringRef fname = func->getName();
        if (fname=="capable")
        {
            chk_func_cap_position[func] = 0;
        }else if (fname=="ns_capable")
        {
            chk_func_cap_position[func] = 1;
        }
        if (chk_func_cap_position.size()==2)
            break;//we are done here
    }

    for (Module::iterator fi = module.begin(), f_end = module.end();
            fi != f_end; ++fi)
    {
        Function *func = dyn_cast<Function>(fi);
        if (func->isDeclaration() || func->isIntrinsic()
                ||!is_function_chk_or_wrapper(func))
            continue;
        int cap_pos = chk_func_cap_position[func];
        assert(cap_pos>=0);
        //we got a capability check function or a wrapper function,
        //find all use without Constant Value and add them to wrapper
        for (auto U: func->users())
        {
            CallInst* cs = dyn_cast<CallInst>(U);
            if (cs==NULL)
                continue;//how come?
            assert(cs->getCalledFunction()==func);
            Value* capv = cs->getArgOperand(cap_pos);
            if (isa<ConstantInt>(capv))
                continue;
            Function* parent_func = cs->getFunction();
            //we have a wrapper,
            int pos = use_parent_func_arg(capv, parent_func);
            if (pos>=0)
            {
                //type 1 wrapper, cap is from parent function argument
                chk_func_cap_position[parent_func] = pos;
            }else
            {
                //type 2 wrapper, cap is from inside this function
                //what to do with this?
            }
        }
    }

    dump_chk_and_wrap();
}

void capchk::collect_pp(Module& module)
{
    errs()<<"Collect all functions and indirect callsites\n";
    for (Module::iterator fi = module.begin(), f_end = module.end();
            fi != f_end; ++fi)
    {
        Function *func = dyn_cast<Function>(fi);
        if (func->isDeclaration() || func->isIntrinsic())
            continue;

        all_functions.insert(func);
        Type* type = func->getFunctionType();
        FunctionSet *fl = t2fs[type];
        if (fl==NULL)
        {
            fl = new FunctionSet;
            t2fs[type] = fl;
        }
        fl->insert(func);

        for(Function::iterator fi = func->begin(), fe = func->end();
                fi != fe; ++fi)
        {
            BasicBlock* bb = dyn_cast<BasicBlock>(fi);
            for (BasicBlock::iterator ii = bb->begin(), ie = bb->end(); ii!=ie; ++ii)
            {
                CallInst* ci = dyn_cast<CallInst>(ii);
                if (!ci || ci->getCalledFunction() || ci->isInlineAsm())
                    continue;
                idcs.insert(ci);
            }
        }
    }
}

void capchk::collect_crits(Module& module)
{
    for (Module::iterator fi = module.begin(), f_end = module.end();
            fi != f_end; ++fi)
    {
        Function *func = dyn_cast<Function>(fi);
        if (func->isDeclaration())
        {
            ExternalFuncCounter++;
            continue;
        }
        if (func->isIntrinsic() || is_function_chk_or_wrapper(func))
            continue;

        FuncCounter++;
        dbgstk.push_back(func->getEntryBlock().getFirstNonPHI());
        InstructionList callgraph;
        InstructionList chks;
        forward_all_interesting_usage(func->getEntryBlock().getFirstNonPHI(),
               0, false, callgraph, chks);
        dbgstk.pop_back();
    }

    critical_variables.sort(cmp_llvm_val);
    critical_variables.erase(
            std::unique(critical_variables.begin(), critical_variables.end()),
            critical_variables.end());

    STOP_WATCH_STOP;
    STOP_WATCH_REPORT;

    CRITFUNC = critical_functions.size();
    CRITVAR = critical_variables.size();
}

_REACHABLE capchk::backward_slice_build_callgraph(InstructionList &callgraph,
            Instruction* I, FunctionToCheckResult& fvisited)
{
    Function* f = I->getFunction();
    _REACHABLE ret = RNONE;

    if (fvisited.find(f)!=fvisited.end())
        return fvisited[f];
#if 0
    errs()<<"+ "<<callgraph.size();
    for (int i=0;i<callgraph.size();i++)
        errs()<<" ";
    errs()<<f->getName()<<"\n";
#endif
    //place holder
    fvisited[f] = RNONE;
    
    DominatorTree dt(*f);

    callgraph.push_back(I);
    //run backward slicing within this function to see if there's a check

    bool checked = false;
    bool has_check = false;
    bool has_no_check = false;
    bool has_user = false;

    InstructionSet* chks = f2chks[f];

    if (is_kernel_init_functions(f))
    {
        ret = RKINIT;
        goto checked_out; 
    }
////////////////////////////////////////////////////////////////////////////////
    //all predecessor basic blocks
    //should call check function 
    //Also check whether the check can dominate use
    if (chks!=NULL)
    {
        for (auto* chk: *chks)
        {
            if (dt.dominates(chk, I))
            {
                errs()<<ANSI_COLOR(BG_GREEN, FG_BLACK)
                    <<"Hit Check Function:"
                    <<get_callee_function_name(chk)
                    <<" @ ";
                chk->getDebugLoc().print(errs());
                errs()<<ANSI_COLOR_RESET<<"\n";

                ret = RFULL;
                has_check = true;
                goto checked_out;
            }else
            {
                has_no_check = true;
            }
        }
    //FIXME: should consider check inside other Callee used by this function
    }
////////////////////////////////////////////////////////////////////////////////
    //if no check and not entry function?
    //we need to go further if not reaching limit ye
    if (callgraph.size()>MAX_BACKWD_SLICE_DEPTH)
    {
        BwdAnalysisMaxHit++;
        ret = RNONE;
        goto nocheck_out;
    }
    //Check function user
    //Direct CallSite
    for (auto *U: f->users())
    {
        has_user = true;
        if (isa<CallInst>(U))
        {
            switch (backward_slice_build_callgraph(callgraph,
                                            dyn_cast<Instruction>(U), fvisited))
            {
                case RFULL:
                    has_check = true;
                    break;
                case RPARTIAL:
                    has_check = true;
                    has_no_check = true;
                    break;
                case RNONE:
                    has_no_check = true;
                    break;
                case RUNRESOLVEABLE:
                    break;
                case RKINIT:
                    break;
                default:
                    llvm_unreachable("what????");
                    break;
            }
        }else
            CFuncUsedByNonCall++;
    }
    //indirect CallSite
    switch(bs_using_indcs(f, callgraph, fvisited))
    {
        case RFULL:
            has_check = true;
            break;
        case RPARTIAL:
            has_check = true;
            has_no_check = true;
            break;
        case RNONE:
            has_no_check = true;
            break;
        case RUNRESOLVEABLE:
            break;
        case RKINIT:
            break;
        default:
            llvm_unreachable("what????");
            break;
    }
    //other indirect call site using this function?
    
    if (!has_user)
    {
        errs()<<" @ "<<f->getName()<<" ";
        errs()<<"Not KInit/CallSite? consider as user entry point\n";
        ret = RNONE;
        goto nocheck_out;
    }

    callgraph.pop_back();
    if (has_check)
    {
        if (has_no_check)
        {
            fvisited[f] = RPARTIAL;
            return RPARTIAL;
        }
        fvisited[f] = RFULL;
        return RFULL;
    }
    fvisited[f] = RNONE;
    return RNONE;

nocheck_out:
    errs()<<"\n"<<ANSI_COLOR_RED
        <<"=NO CHECK ON PATH="
        <<ANSI_COLOR_RESET<<"\n";
    dump_callstack(callgraph);
    errs()<<ANSI_COLOR_RESET;
    callgraph.pop_back();
    fvisited[f] = RNONE;
    return ret;

checked_out:
    if (ret==RFULL)//dont print if it hit kernel init functions
    {
        errs()<<ANSI_COLOR_GREEN
            <<"=PATH OK ret:"<<ret<<"="
            <<ANSI_COLOR_RESET<<"\n";
        dump_callstack(callgraph);
    }
    callgraph.pop_back();
    return ret;
}

_REACHABLE capchk::_backward_slice_reachable_to_chk_function(Instruction* I)
{
    InstructionList callgraph;
    //FIXME: should consider function+instruction pair?
    FunctionToCheckResult fvisited;
    return backward_slice_build_callgraph(callgraph, I, fvisited);
}

void capchk::backward_slice_reachable_to_chk_function(Instruction* cs)
{
    errs()<<ANSI_COLOR_MAGENTA
        <<"Use:";
    cs->getDebugLoc().print(errs());
    //U->print(errs());
    errs()<<ANSI_COLOR_RESET<<"\n";
    switch(_backward_slice_reachable_to_chk_function(cs))
    {
        case(RFULL):
            GoodPath++;
            errs()<<ANSI_COLOR_GREEN
                <<"[FULLY CHECKED]"<<ANSI_COLOR_RESET<<"\n";
            break;
        case(RPARTIAL):
            BadPath++;
            errs()<<ANSI_COLOR_YELLOW
                <<"[PARTIALLY CHECKED]"<<ANSI_COLOR_RESET<<"\n";
            break;
        case(RNONE):
            BadPath++;
            errs()<<ANSI_COLOR_RED
                <<"[NO CHECK]"<<ANSI_COLOR_RESET<<"\n";
            break;
        case(RUNRESOLVEABLE):
            UnResolv++;
            break;
        case(RKINIT):
            GoodPath++;
            errs()<<ANSI_COLOR_CYAN
                <<"[KINIT]"<<ANSI_COLOR_RESET<<"\n";
            break;
        default:
            llvm_unreachable("What???");
            break;
    }
}
/*
 * exact match with bitcast
 */
_REACHABLE capchk::match_cs_using_fptr_method_0(Function* func,
                InstructionList& callgraph, FunctionToCheckResult& visited)
{
    _REACHABLE ret = RNONE;
    int cnt = 0;
    for (auto* idc: idcs)
    {
        Value* cv = idc->getCalledValue();
        //or strip function pointer can do the trick?
        Function* _func = dyn_cast<Function>(cv->stripPointerCasts());
        if (_func==func)
        {
            cnt++;
            MatchCallCriticalFuncPtr++;
            errs()<<"Found matched functions(bitcast) for call-by-val:"
                <<func->getName()<<"\n";
            //FIXME return value
            ret = backward_slice_build_callgraph(callgraph, idc, visited);
            continue;
        }
    }
end:
    if (cnt!=0)
        UnMatchCallCriticalFuncPtr++;
    return ret;
}

/*
 * signature based method to find out indirect callee
 */
_REACHABLE capchk::match_cs_using_fptr_method_1(Function* func,
                InstructionList& callgraph, FunctionToCheckResult& visited)
{
    //we want exact match to non-trivial function
    _REACHABLE ret = RNONE;
    int cnt = 0;
    Type* func_type = func->getFunctionType();
    FunctionSet *fl = t2fs[func_type];
    if (!is_complex_type(func_type))
        goto end;
    if ((fl==NULL) || (fl->size()!=1))
        goto end;
    if ((*fl->begin())!=func)
        goto end;
    for (auto* idc: idcs)
    {
        Value* cv = idc->getCalledValue();
        //or strip function pointer can do the trick?
        Function* _func = dyn_cast<Function>(cv->stripPointerCasts());
        if (_func==func)
            continue;

        Type* ft = cv->getType()->getPointerElementType();
        Type* ft2 = cv->stripPointerCasts()->getType()->getPointerElementType();
        if ((func_type == ft) || (func_type == ft2))
        {
            cnt++;
            MatchCallCriticalFuncPtr++;
            errs()<<"Found matched functions for indirectcall:"
                <<(*fl->begin())->getName()<<"\n";
            ret = backward_slice_build_callgraph(callgraph, idc, visited);
            continue;
        }
    }
end:
    if (cnt!=0)
        UnMatchCallCriticalFuncPtr++;
    return ret;
}

/*
 * TODO: figure out function pointer using SVF
 */
_REACHABLE capchk::match_cs_using_fptr_method_2(Function* func,
                InstructionList& callgraph, FunctionToCheckResult& visited)
{
    Type* func_type = func->getFunctionType();
    for (auto* idc: idcs)
    {
        Value* cv = idc->getCalledValue();
        Function* _func = dyn_cast<Function>(cv->stripPointerCasts());
        if (_func==func)
            continue;
    }
    return RNONE;
}

_REACHABLE capchk::bs_using_indcs(Function* func,
                InstructionList& callgraph, FunctionToCheckResult& visited)
{
    _REACHABLE ret;
    //exact match don't need to look further
    ret = match_cs_using_fptr_method_0(func, callgraph, visited);
    if (ret==RFULL)
        return ret;
    ret = (match_cs_using_fptr_method_1(func, callgraph, visited));
    if (ret==RFULL)
        return ret;
    /*if (match_cs_using_fptr_method_2(func, callgraph, visited))
    {
        cnt++;
        MatchCallCriticalFuncPtr++;
        return;
    }*/
    return ret;
}

/*
 * Complex Value Flow Analysis
 * figure out candidate for indirect callee using value flow analysis
 *
 * TODO: refactor method0~2, using following routine to pre-calculate
 * candidate function so that we can make check simpler
 *
 */
void capchk::resolve_all_indirect_callee(Module& module)
{
    //collect all function pointer assignment(fptrassign=source)
    for (Module::iterator mi = module.begin(), me = module.end(); mi != me; ++mi)
    {
        Function *func = dyn_cast<Function>(mi);
        if (func->isDeclaration() || func->isIntrinsic())
            continue;
        for (auto* U: func->users())
        {
            Value* u = dyn_cast<Value>(U);
            if (isa<CallInst>(U))
                continue;
            //not interested in pure bitcast?
            if (Instruction* i = dyn_cast<Instruction>(u))
                fptrassign.insert(i);
        }
    }

    //create svf instance
    CVFA cvfa;
    //initialize, this will take some time
    cvfa.initialize(module);

    //do analysis(idcs=sink)
    //method 1, simple type cast
    for (auto* idc: idcs)
    {
        Value* cv = idc->getCalledValue();

        FunctionSet* funcs = idcs2callee[idc];
        if (funcs==NULL)
        {
            funcs = new FunctionSet;
            idcs2callee[idc] = funcs;
        }
        if (Function* func = dyn_cast<Function>(cv->stripPointerCasts()))
        {
            funcs->insert(func);
            continue;
        }
    }
    //method 2, value flow, track down def-use-chain till we found
    //function pointer assignment
    if (knob_capchk_cvf)
    {
        errs()<<"SVF indirect call track:\n";
        for (auto f: all_functions)
        {
            std::set<const Instruction*> css;
            cvfa.get_indirect_callee_for_func(f, css);
            if (css.size()==0)
                continue;
            errs()<<"FUNC:"<<f->getName()<<", found "<<css.size()<<"\n";
            for (auto* _ci: css)
            {
                const CallInst* ci = dyn_cast<CallInst>(_ci);
                FunctionSet* funcs = idcs2callee[ci];
                if (funcs==NULL)
                {
                    funcs = new FunctionSet;
                    idcs2callee[ci] = funcs;
                }
                funcs->insert(f);
#if 1
                errs()<<"CallSite: ";
                ci->getDebugLoc().print(errs());
                errs()<<"\n";
                ci->print(errs());
                errs()<<"\n";
#endif
            }
        }
    }
}

/*
 * check possible critical function path 
 */
void capchk::check_critical_function_usage(Module& module)
{
    FunctionList processed_flist;
    //for each critical function find out all callsite(use)
    //process one critical function at a time
    for (Module::iterator f_begin = module.begin(), f_end = module.end();
            f_begin != f_end; ++f_begin)
    {
        Function *func = dyn_cast<Function>(f_begin);
        if (!is_critical_function(func))
            continue;
        if (is_skip_function(func->getName()))
            continue;
        errs()<<ANSI_COLOR_YELLOW
            <<"Inspect Use of Function:"
            <<func->getName()
            <<ANSI_COLOR_RESET
            <<"\n";
        //iterate through all call site
        //direct call
        for (auto *U: func->users())
        {
            CallInst *cs = dyn_cast<CallInst>(U);
            if (!cs)
                continue;
            backward_slice_reachable_to_chk_function(cs);
        }
    }
}



/*
 * run inter-procedural backward analysis to figure out whether this can
 * be reached from entry point without running check
 *
 * FIXME: check critical variable usage is different, 
 * for global variable ned to scan all instructions and see if it tracks down
 * to a critical global variable
 *
 */
void capchk::check_critical_variable_usage(Module& module)
{
    for (auto *V: critical_variables)
    {
        FunctionList flist;//known functions
#if DEBUG_ANALYZE
        errs()<<ANSI_COLOR_YELLOW
            <<"Inspect Use of Variable:"
            <<V->getName()
            <<ANSI_COLOR_RESET<<"\n";
#endif
        for (auto *U: V->users())
        {
            Instruction *ui = dyn_cast<Instruction>(U);
            if (!ui)//not an instruction????
                continue;
            Function* f = dyn_cast<Instruction>(U)->getFunction();

            //make sure this is not a kernel init function
            if (is_kernel_init_functions(f))
                continue;
            if (isa<LoadInst>(ui))
            {
                errs()<<"LOAD: ";
            }else if (isa<StoreInst>(ui))
            {
                errs()<<"STORE: ";
            }else
            {
                errs()<<"Use:opcode="<<ui->getOpcode()<<" ";
            }
            ui->getDebugLoc().print(errs());
            errs()<<"\n";

            flist.push_back(f);
            errs()<<"Function: "
                <<f->getName()
                <<"\n";
            //is this instruction reachable from non-checked path?
            switch(_backward_slice_reachable_to_chk_function(dyn_cast<Instruction>(U)))
            {
                case(RFULL):
                    GoodPath++;
                    errs()<<ANSI_COLOR_GREEN
                        <<"[FULL]"<<ANSI_COLOR_RESET<<"\n";
                    break;
                case(RPARTIAL):
                    BadPath++;
                    errs()<<ANSI_COLOR_YELLOW
                        <<"[PARTIAL]"<<ANSI_COLOR_RESET<<"\n";
                    break;
                case(RNONE):
                    BadPath++;
                    errs()<<ANSI_COLOR_RED
                        <<"[NONE]"<<ANSI_COLOR_RESET<<"\n";
                    break;
                case(RUNRESOLVEABLE):
                    UnResolv++;
                    break;
                case(RKINIT):
                    GoodPath++;
                    break;
                default:
                    llvm_unreachable("what?????");
                    break;
            }
        }
    }
}

/*
 * IPA: figure out all global variable usage and function calls
 */
void capchk::forward_all_interesting_usage(Instruction* I, int depth,
        bool checked, InstructionList& callgraph,
        InstructionList& chks)
{
    Function *func = I->getFunction();
    DominatorTree dt(*func);

    bool is_function_permission_checked = checked;
    /*
     * collect interesting variables/functions found in this function
     */
    FunctionSet current_crit_funcs;
    ValueList current_critical_variables;

    //don't allow recursive
    if (std::find(callgraph.begin(), callgraph.end(), I)!=callgraph.end())
        return;

    callgraph.push_back(I);

    if (depth>MAX_FWD_SLICE_DEPTH)
    {
        callgraph.pop_back();
        FwdAnalysisMaxHit++;
        return;
    }

    BasicBlockSet bb_visited;
    std::queue<BasicBlock*> bb_work_list;

    /*
     * a list of instruction where check functions are used,
     * that will be later used to do dominance checking
     */
    InstructionList chk_instruction_list;

/*****************************
 * first figure out all checks
 */
    //already checked?
    if (is_function_permission_checked)
        goto rescan_and_add_all;

    bb_work_list.push(I->getParent());
    while(bb_work_list.size())
    {
        BasicBlock* bb = bb_work_list.front();
        bb_work_list.pop();
        if(bb_visited.count(bb))
            continue;
        bb_visited.insert(bb);

        for (BasicBlock::iterator ii = bb->begin(),
                ie = bb->end();
                ii!=ie; ++ii)
        {
            if (isa<CallInst>(ii))
            {
                //only interested in call site
                CallInst *ci = dyn_cast<CallInst>(ii);
                Function* csfunc = ci->getCalledFunction();
                if (csfunc && csfunc->hasName())
                {
                    if (csfunc->isIntrinsic()
                            ||is_skip_function(csfunc->getName()))
                        continue;

                    if (is_function_chk_or_wrapper(csfunc))
                    {
                        is_function_permission_checked = true;
                        chk_instruction_list.push_back(ci);
                        chks.push_back(ci);
                    }
                }else
                {
                    //this is in-direct call, collect it
                    //don't really care inline asm
                    if (!ci->isInlineAsm())
                    {
                    }
                }
            }
        }
        //insert all successor of current basic block to work list
        for (succ_iterator si = succ_begin(bb), se = succ_end(bb); si!=se; ++si)
            bb_work_list.push(cast<BasicBlock>(*si));
    }

    if (!is_function_permission_checked)
        goto out;

/*******************************************************************
 * second, re-scan all instructions and figure out 
 * which one can be dominated by those check instructions(protected)
 */
rescan_and_add_all:
    bb_work_list.push(I->getParent());
    bb_visited.clear();
    while(bb_work_list.size())
    {
        BasicBlock* bb = bb_work_list.front();
        bb_work_list.pop();
        if(bb_visited.count(bb))
            continue;
        bb_visited.insert(bb);

        for (BasicBlock::iterator ii = bb->begin(), ie = bb->end(); ii!=ie; ++ii)
        {
            Instruction* si = dyn_cast<Instruction>(ii);
            /*
             * if any check dominate si then go ahead and
             * add them to protected list
             */
            //already checked before entering current scope
            //all following usage should be dominated by incoming Instruction
            if (checked)
            {
                //should dominate use
                if (dt.dominates(I,si))
                    goto add;
                //or should have newly discovered check..
            }

            //otherwise, there should be at least one check dominate the use
            for (auto* _ci : chk_instruction_list)
                if (dt.dominates(_ci,si))
                    goto add;
            //dont care if not protected
            continue;

add:
            if (isa<CallInst>(ii))
            {
                CallInst* cs = dyn_cast<CallInst>(ii);
                Function* csf = cs->getCalledFunction();
                //ignore inline asm
                if (cs->isInlineAsm())
                    continue;
                //simple type cast?
                if (csf==NULL)
                        csf = dyn_cast<Function>(cs->getCalledValue()
                                                    ->stripPointerCasts());

                if (csf!=NULL)
                {
                    if (csf->isIntrinsic()
                            ||is_skip_function(csf->getName())
                            ||is_function_chk_or_wrapper(csf)
                            )
                        continue;
                    current_crit_funcs.insert(csf);

                    if (knob_capchk_ccfv)
                    {
                        errs()<<"Add call<direct> "<<csf->getName()<<" use @ ";
                        cs->getDebugLoc().print(errs());
                        errs()<<"\n cause:";
                        dump_dbgstk();
                    }

                    InstructionSet* ill = f2ci[csf];
                    if (ill==NULL)
                    {
                        ill = new InstructionSet;
                        f2ci[csf] = ill;
                    }
                    for (auto chki: chks)
                        ill->insert(chki);

                }else if (Value* csv = cs->getCalledValue())
                {
                    errs()<<"Want to resolve indirect call @ ";
                    cs->getDebugLoc().print(errs());
                    errs()<<"\n";

                    FunctionSet fs = resolve_indirect_callee(cs);
                    if (!fs.size())
                    {
                        CPUnResolv++;
                        continue;
                    }
                    CPResolv++;
                    Function* csf = *fs.begin();
#if 1
                    StringRef fname = csf->getName();
                    if (csf->isIntrinsic()
                            ||is_skip_function(fname)
                            ||is_function_chk_or_wrapper(csf))
                        continue;

                    current_crit_funcs.insert(csf);
                    if (knob_capchk_ccfv)
                    {
                        errs()<<"Add call<indirect> "<<csf->getName()<<" use @ ";
                        cs->getDebugLoc().print(errs());
                        errs()<<"\n cause:";
                        dump_dbgstk();
                    }
                    InstructionSet* ill = f2ci[csf];
                    if (ill==NULL)
                    {
                        ill = new InstructionSet;
                        f2ci[csf] = ill;
                    }
                    //insert all chks?
                    for (auto chki: chks)
                        ill->insert(chki);
#endif
                }
            }
            /*
             * load/store from/to global variable will be considered
             * critical variable
             */
            else if (isa<LoadInst>(ii))
            {
                LoadInst *li = dyn_cast<LoadInst>(ii);
                Value* lval = li->getOperand(0);
                Value* gv = get_global_def(lval);
                if (gv && (!is_skip_var(gv->getName())))
                {
                    if (knob_capchk_ccvv)
                    {
                        errs()<<"Add Load "<<gv->getName()<<" use @ ";
                        li->getDebugLoc().print(errs());
                        errs()<<"\n cause:";
                        dump_dbgstk();
                    }
                    current_critical_variables.push_back(lval);
                    InstructionSet* ill = v2ci[gv];
                    if (ill==NULL)
                    {
                        ill = new InstructionSet;
                        v2ci[lval] = ill;
                    }
                    for (auto chki: chks)
                        ill->insert(chki);
                }
            }else if (isa<StoreInst>(ii))
            {
                StoreInst *si = dyn_cast<StoreInst>(ii);
                Value* sval = si->getOperand(1);
                Value* gv = get_global_def(sval);
                if (gv && (!is_skip_var(gv->getName())))
                {
                    if (knob_capchk_ccvv)
                    {
                        errs()<<"Add Store "<<gv->getName()<<" use @ ";
                        si->getDebugLoc().print(errs());
                        errs()<<"\n cause:";
                        dump_dbgstk();
                    }
                    current_critical_variables.push_back(sval);
                    InstructionSet* ill = v2ci[gv];
                    if (ill==NULL)
                    {
                        ill = new InstructionSet;
                        v2ci[sval] = ill;
                    }
                    for (auto chki: chks)
                        ill->insert(chki);
                }
            }
        }
        for (succ_iterator si = succ_begin(bb), se = succ_end(bb); si!=se; ++si)
            bb_work_list.push(cast<BasicBlock>(*si));
    }
    /**********
     * merge 
     */
    if (is_function_permission_checked)
    {
        //merge forwar slicing result
        for (auto i: current_crit_funcs)
            critical_functions.insert(i);

        //if (current_critical_variables.size()==0)
        //{
        //    errs()<<"No global critical variables found for function?:"
        //        <<func->getName()<<"\n";
        //}
        critical_variables.splice(critical_variables.begin(),
                current_critical_variables);

        //if functions is permission checked, consider this as a wrapper
        //and we need to check all use of this function
        for (auto *U: func->users())
        {
            if (CallInst* cs = dyn_cast<CallInst>(U))
            {
                Function* pfunc = cs->getFunction();
                if (pfunc->isIntrinsic())
                    continue;
                if (is_kernel_init_functions(pfunc))
                {
                    dbgstk.push_back(cs);
                    errs()<<ANSI_COLOR_YELLOW
                        <<"capability check used during kernel initialization\n"
                        <<ANSI_COLOR_RESET;
                    dump_dbgstk();
                    dbgstk.pop_back();
                    continue;
                }

                dbgstk.push_back(cs);
                forward_all_interesting_usage(cs, depth+1, true, callgraph, chks);
                dbgstk.pop_back();
            }
        }
    }
out:
    callgraph.pop_back();
    return;
}

/*
 * capability protected escaping variables and fields - intra-procedural
 * (field sensitive?)
 *
 * TODO:
 * 1. can collect protected type/field and extend analysis to other functions
 * 2. check wrapper
 *
 * rules:
 * 1. capability check should dominate >0 store to escaping variable
 *    or return with value
 * 2. capability check should cover all pathes which try to do the same thing
 */
void capchk::process_intras(Module& module)
{
    STOP_WATCH_START;
    for (Module::iterator fi = module.begin(), fe = module.end();
            fi != fe; ++fi)
    {
        Function *func = dyn_cast<Function>(fi);
        if (func->isDeclaration() || func->isVarArg())
            continue;

        FunctionType* type = func->getFunctionType();
        //is there any (escaping)pointer argument? return value?
        //if no, we should expect global variables are modified/referenced
        for (int i=0; i < type->getNumParams(); i++)
            if (type->getParamType(i)->isPointerTy())
                goto interesting;
        if (type->getReturnType()->isVoidTy())
            continue;

interesting:
        bool good = true;
        //auto& aa = getAnalysis<AAResultsWrapperPass>(*func).getAAResults();


        std::set<BasicBlock*> bb_visited;
        std::queue<BasicBlock*> bb_work_list;
        bb_work_list.push(&func->getEntryBlock());
        InstructionList chk_ins;
//phase-1, is check function used?
        while(bb_work_list.size())
        {
            BasicBlock* bb = bb_work_list.front();
            bb_work_list.pop();
            if(bb_visited.count(bb))
                continue;
            bb_visited.insert(bb);

            for (BasicBlock::iterator ii = bb->begin(),
                    ie = bb->end();
                    ii!=ie; ++ii)
            {
                CallInst* ci = dyn_cast<CallInst>(ii);
                if (!ci)
                    continue;
                Function* nf = ci->getCalledFunction();
                if (!nf)
                    continue;
                if (!is_check_function(nf->getName()))
                    continue;
                chk_ins.push_back(ci);
            }
            for (succ_iterator si = succ_begin(bb), se = succ_end(bb); si!=se; ++si)
                bb_work_list.push(cast<BasicBlock>(*si));
        }
        bb_visited.clear();
        if (chk_ins.size()==0)
            continue;
        errs()<<"-"<<func->getName()<<"\n";
        DominatorTree dt(*func);
//phase-2, intersect def-use chain for args
//store only
        for (Function::arg_iterator fai = func->arg_begin(), fae = func->arg_end();
            fai!=fae; ++fai)
        {
            //find out all fai's user
            ValueList fai_user;
            ValueList wl;
            wl.push_back(fai);
            while (wl.size())
            {
                Value* v = wl.front();
                wl.pop_front();
                for (auto* u: v->users())
                {
                    //if already in fai_user, don't add
                    if (std::find(fai_user.begin(), fai_user.end(), u)
                            != fai_user.end())
                        continue;
                    wl.push_back(u);
                    fai_user.push_back(u);
                }
            }
            //is fai_user partially dominated by check?
            InstructionList uil;
            for (auto* u:fai_user)
            {
                if (!isa<StoreInst>(u))
                    continue;
                int domcnt = 0;
                for (auto* ci: chk_ins)
                {
                    if (dt.dominates(ci, dyn_cast<Instruction>(u)))
                        domcnt++;
                }
                if (domcnt==0)//this path is not checked
                    uil.push_back(dyn_cast<Instruction>(u));
            }
            for (auto* ui: uil)
            {
                errs()<<"    "
                    <<ANSI_COLOR_YELLOW
                    <<"NOCHK @ "
                    <<ANSI_COLOR_RESET;
                assert(isa<Instruction>(ui));
                ui->getDebugLoc().print(errs());
                errs()<<"\n";
                good = false;
            }
        }
//phase-3, intersect def-use chain for return value
//unchecked variable have impact on return value
        if (good)
        {
            errs()<<ANSI_COLOR_GREEN<<"[OK]"<<ANSI_COLOR_RESET<<"\n";
        }else{
            errs()<<ANSI_COLOR_RED<<"[FAIL]"<<ANSI_COLOR_RESET<<"\n";
        }
    }
    STOP_WATCH_STOP;
    STOP_WATCH_REPORT;
}

/*
 * process capability protected globals and functions
 */
void capchk::process_cpgf(Module& module)
{
    /*
     * pre-process
     * generate resource/functions from syscall entry function
     */
    errs()<<"Pre-processing...\n";
    STOP_WATCH_START;
    collect_pp(module);
    STOP_WATCH_STOP;
    STOP_WATCH_REPORT;

    errs()<<"Identify wrappers\n";
    STOP_WATCH_START;
    collect_wrappers(module);
    STOP_WATCH_STOP;
    STOP_WATCH_REPORT;

    errs()<<"Collect Checkpoints\n";
    STOP_WATCH_START;
    collect_chkps(module);
    STOP_WATCH_STOP;
    STOP_WATCH_REPORT;

    if (knob_capchk_cvf)
    {
        errs()<<"Resolving callee for indirect call.\n";
        STOP_WATCH_START;
        resolve_all_indirect_callee(module);
        STOP_WATCH_STOP;
        STOP_WATCH_REPORT;
    }

    errs()<<"Collecting Initialization Closure.\n";
    STOP_WATCH_START;
    collect_kernel_init_functions(module);
    STOP_WATCH_STOP;
    STOP_WATCH_REPORT;

    errs()<<"Collect all permission-checked variables and functions\n";
    STOP_WATCH_START;
    collect_crits(module);
    errs()<<"Collected "<<critical_functions.size()<<" critical functions\n";
    errs()<<"Collected "<<critical_variables.size()<<" critical variables\n";

    dump_v2ci();
    dump_f2ci();

    errs()<<"Run Analysis\n";
    if (knob_capchk_critical_var)
    {
        errs()<<"Critical variables\n";
        STOP_WATCH_START;
        check_critical_variable_usage(module);
        STOP_WATCH_STOP;
        STOP_WATCH_REPORT;
    }
    if (knob_capchk_critical_fun)
    {
        errs()<<"Critical functions\n";
        STOP_WATCH_START;
        check_critical_function_usage(module);
        STOP_WATCH_STOP;
        STOP_WATCH_REPORT;
    }
    dump_non_kinit();
}

bool capchk::runOnModule(Module &module)
{
    m = &module;
    return capchkPass(module);
}

bool capchk::capchkPass(Module &module)
{

#if 0
    errs()<<ANSI_COLOR_CYAN
        <<"--- UNSAFE ACCESS CHECKER ---"
        <<ANSI_COLOR_RESET<<"\n";
    chk_unsafe_access(module);
#endif

#if 0
    errs()<<ANSI_COLOR_CYAN
        <<"--- MAY DIV BY ZERO CHECKER ---"
        <<ANSI_COLOR_RESET<<"\n";
    chk_div0(module);
#endif
    errs()<<ANSI_COLOR_CYAN
        <<"--- PROCESS FUNCTIONS ---"
        <<ANSI_COLOR_RESET<<"\n";
    //process_intras(module);

    process_cpgf(module);

    errs()<<ANSI_COLOR_CYAN
        <<"--- DONE! ---"
        <<ANSI_COLOR_RESET<<"\n";

#if CUSTOM_STATISTICS
    dump_statistics();
#endif
    return false;
}

static RegisterPass<capchk>
XXX("capchk", "capchk Pass (with getAnalysisUsage implemented)");


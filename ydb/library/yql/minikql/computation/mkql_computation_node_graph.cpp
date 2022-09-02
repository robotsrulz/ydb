#include "mkql_computation_node_holders.h"
#include "mkql_value_builder.h"
#include "mkql_computation_node_codegen.h"
#include <ydb/library/yql/minikql/arrow/mkql_memory_pool.h>
#include <ydb/library/yql/minikql/comp_nodes/mkql_saveload.h>
#include <ydb/library/yql/minikql/mkql_type_builder.h>
#include <ydb/library/yql/minikql/mkql_terminator.h>
#include <ydb/library/yql/minikql/mkql_string_util.h>
#include <util/system/env.h>
#include <util/system/mutex.h>
#include <util/digest/city.h>

#ifndef MKQL_DISABLE_CODEGEN
#include <llvm/Support/raw_ostream.h>
#endif

namespace NKikimr {
namespace NMiniKQL {

using namespace NDetail;

namespace {

#ifndef MKQL_DISABLE_CODEGEN
constexpr ui64 TotalFunctionsLimit = 1000;
constexpr ui64 TotalInstructionsLimit = 100000;
constexpr ui64 MaxFunctionInstructionsLimit = 50000;
#endif

const ui64 IS_NODE_REACHABLE = 1;

const static TStatKey PagePool_PeakAllocated("PagePool_PeakAllocated", false);
const static TStatKey PagePool_PeakUsed("PagePool_PeakUsed", false);
const static TStatKey PagePool_AllocCount("PagePool_AllocCount", true);
const static TStatKey PagePool_PageAllocCount("PagePool_PageAllocCount", true);
const static TStatKey PagePool_PageHitCount("PagePool_PageHitCount", true);
const static TStatKey PagePool_PageMissCount("PagePool_PageMissCount", true);
const static TStatKey PagePool_OffloadedAllocCount("PagePool_OffloadedAllocCount", true);
const static TStatKey PagePool_OffloadedBytes("PagePool_OffloadedBytes", true);

const static TStatKey CodeGen_FullTime("CodeGen_FullTime", true);
const static TStatKey CodeGen_GenerateTime("CodeGen_GenerateTime", true);
const static TStatKey CodeGen_CompileTime("CodeGen_CompileTime", true);
const static TStatKey CodeGen_TotalFunctions("CodeGen_TotalFunctions", true);
const static TStatKey CodeGen_TotalInstructions("CodeGen_TotalInstructions", true);
const static TStatKey CodeGen_MaxFunctionInstructions("CodeGen_MaxFunctionInstructions", false);
const static TStatKey CodeGen_ModulePassTime("CodeGen_ModulePassTime", true);
const static TStatKey CodeGen_FinalizeTime("CodeGen_FinalizeTime", true);

const static TStatKey Mkql_TotalNodes("Mkql_TotalNodes", true);
const static TStatKey Mkql_CodegenFunctions("Mkql_CodegenFunctions", true);

class TDependencyScanVisitor : public TEmptyNodeVisitor {
public:
    void Walk(TNode* root, const TTypeEnvironment& env) {
        Stack = &env.GetNodeStack();
        Stack->clear();
        Stack->push_back(root);
        while (!Stack->empty()) {
            auto top = Stack->back();
            Stack->pop_back();
            if (top->GetCookie() != IS_NODE_REACHABLE) {
                top->SetCookie(IS_NODE_REACHABLE);
                top->Accept(*this);
            }
        }

        Stack = nullptr;
    }

private:
    using TEmptyNodeVisitor::Visit;

    void Visit(TStructLiteral& node) override {
        for (ui32 i = 0; i < node.GetValuesCount(); ++i) {
            AddNode(node.GetValue(i).GetNode());
        }
    }

    void Visit(TListLiteral& node) override {
        for (ui32 i = 0; i < node.GetItemsCount(); ++i) {
            AddNode(node.GetItems()[i].GetNode());
        }
    }

    void Visit(TOptionalLiteral& node) override {
        if (node.HasItem()) {
            AddNode(node.GetItem().GetNode());
        }
    }

    void Visit(TDictLiteral& node) override {
        for (ui32 i = 0; i < node.GetItemsCount(); ++i) {
            AddNode(node.GetItem(i).first.GetNode());
            AddNode(node.GetItem(i).second.GetNode());
        }
    }

    void Visit(TCallable& node) override {
        if (node.HasResult()) {
            AddNode(node.GetResult().GetNode());
        } else {
            for (ui32 i = 0; i < node.GetInputsCount(); ++i) {
                AddNode(node.GetInput(i).GetNode());
            }
        }
    }

    void Visit(TAny& node) override {
        if (node.HasItem()) {
            AddNode(node.GetItem().GetNode());
        }
    }

    void Visit(TTupleLiteral& node) override {
        for (ui32 i = 0; i < node.GetValuesCount(); ++i) {
            AddNode(node.GetValue(i).GetNode());
        }
    }

    void Visit(TVariantLiteral& node) override {
        AddNode(node.GetItem().GetNode());
    }


    void AddNode(TNode* node) {
        if (node->GetCookie() != IS_NODE_REACHABLE) {
            Stack->push_back(node);
        }
    }

    std::vector<TNode*>* Stack = nullptr;
};

class TPatternNodes: public TAtomicRefCount<TPatternNodes> {
public:
    typedef TIntrusivePtr<TPatternNodes> TPtr;

    TPatternNodes(TAllocState& allocState)
        : AllocState(allocState)
        , MemInfo(MakeIntrusive<TMemoryUsageInfo>("ComputationPatternNodes"))
    {
#ifndef NDEBUG
        AllocState.ActiveMemInfo.emplace(MemInfo.Get(), MemInfo);
#endif
    }

    ~TPatternNodes()
    {
        ComputationNodesList.clear();
        if (!UncaughtException()) {
#ifndef NDEBUG
            AllocState.ActiveMemInfo.erase(MemInfo.Get());
#endif
        }
    }

    const TComputationMutables& GetMutables() const {
        return Mutables;
    }

    const TComputationNodePtrDeque& GetNodes() const {
        return ComputationNodesList;
    }

    IComputationNode* GetComputationNode(TNode* node, bool pop = false, bool require = true) {
        const auto cookie = node->GetCookie();
        const auto result = reinterpret_cast<IComputationNode*>(cookie);

        if (cookie <= IS_NODE_REACHABLE) {
            MKQL_ENSURE(!require, "Computation graph builder, node not found, type:"
                << node->GetType()->GetKindAsStr());
            return result;
        }

        if (pop) {
            node->SetCookie(0);
        }

        return result;
    }

    IComputationExternalNode* GetEntryPoint(size_t index, bool require) {
        MKQL_ENSURE(index < Runtime2Computation.size() && (!require || Runtime2Computation[index]),
            "Pattern nodes can not get computation node by index: " << index << ", require: " << require);
        return Runtime2Computation[index];
    }

    IComputationNode* GetRoot() {
        return RootNode;
    }

private:
    friend class TComputationGraphBuildingVisitor;
    friend class TComputationGraph;

    TAllocState& AllocState;
    TIntrusivePtr<TMemoryUsageInfo> MemInfo;
    THolder<THolderFactory> HolderFactory;
    THolder<TDefaultValueBuilder> ValueBuilder;
    TComputationMutables Mutables;
    TComputationNodePtrDeque ComputationNodesList;
    IComputationNode* RootNode = nullptr;
    TComputationExternalNodePtrVector Runtime2Computation;
    TComputationNodeOnNodeMap ElementsCache;
};

class TComputationGraphBuildingVisitor:
        public INodeVisitor,
        private TNonCopyable
{
public:
    TComputationGraphBuildingVisitor(const TComputationPatternOpts& opts)
        : Env(opts.Env)
        , TypeInfoHelper(new TTypeInfoHelper())
        , CountersProvider(opts.CountersProvider)
        , SecureParamsProvider(opts.SecureParamsProvider)
        , Factory(opts.Factory)
        , FunctionRegistry(*opts.FunctionRegistry)
        , ValidateMode(opts.ValidateMode)
        , ValidatePolicy(opts.ValidatePolicy)
        , GraphPerProcess(opts.GraphPerProcess)
        , PatternNodes(MakeIntrusive<TPatternNodes>(opts.AllocState))
        , ExternalAlloc(opts.CacheAlloc || opts.PatternEnv)
    {
        PatternNodes->HolderFactory = MakeHolder<THolderFactory>(opts.AllocState, *PatternNodes->MemInfo, &FunctionRegistry);
        PatternNodes->ValueBuilder = MakeHolder<TDefaultValueBuilder>(*PatternNodes->HolderFactory, ValidatePolicy);
        PatternNodes->ValueBuilder->SetSecureParamsProvider(opts.SecureParamsProvider);
        NodeFactory = MakeHolder<TNodeFactory>(*PatternNodes->MemInfo, PatternNodes->Mutables);
    }

    ~TComputationGraphBuildingVisitor() {
    }

    const TTypeEnvironment& GetTypeEnvironment() const {
        return Env;
    }

    const IFunctionRegistry& GetFunctionRegistry() const {
        return FunctionRegistry;
    }

private:
    template <typename T>
    void VisitType(T& node) {
        AddNode(node, NodeFactory->CreateTypeNode(&node));
    }

    void Visit(TTypeType& node) override {
        VisitType<TTypeType>(node);
    }

    void Visit(TVoidType& node) override {
        VisitType<TVoidType>(node);
    }

    void Visit(TNullType& node) override {
        VisitType<TNullType>(node);
    }

    void Visit(TEmptyListType& node) override {
        VisitType<TEmptyListType>(node);
    }

    void Visit(TEmptyDictType& node) override {
        VisitType<TEmptyDictType>(node);
    }

    void Visit(TDataType& node) override {
        VisitType<TDataType>(node);
    }

    void Visit(TPgType& node) override {
        VisitType<TPgType>(node);
    }

    void Visit(TStructType& node) override {
        VisitType<TStructType>(node);
    }

    void Visit(TListType& node) override {
        VisitType<TListType>(node);
    }

    void Visit(TStreamType& node) override {
        VisitType<TStreamType>(node);
    }

    void Visit(TFlowType& node) override {
        VisitType<TFlowType>(node);
    }

    void Visit(TBlockType& node) override {
        VisitType<TBlockType>(node);
    }

    void Visit(TTaggedType& node) override {
        VisitType<TTaggedType>(node);
    }

    void Visit(TOptionalType& node) override {
        VisitType<TOptionalType>(node);
    }

    void Visit(TDictType& node) override {
        VisitType<TDictType>(node);
    }

    void Visit(TCallableType& node) override {
        VisitType<TCallableType>(node);
    }

    void Visit(TAnyType& node) override {
        VisitType<TAnyType>(node);
    }

    void Visit(TTupleType& node) override {
        VisitType<TTupleType>(node);
    }

    void Visit(TResourceType& node) override {
        VisitType<TResourceType>(node);
    }

    void Visit(TVariantType& node) override {
        VisitType<TVariantType>(node);
    }

    void Visit(TVoid& node) override {
        AddNode(node, NodeFactory->CreateImmutableNode(NUdf::TUnboxedValue::Void()));
    }

    void Visit(TNull& node) override {
        AddNode(node, NodeFactory->CreateImmutableNode(NUdf::TUnboxedValue()));
    }

    void Visit(TEmptyList& node) override {
        AddNode(node, NodeFactory->CreateImmutableNode(PatternNodes->HolderFactory->GetEmptyContainer()));
    }

    void Visit(TEmptyDict& node) override {
        AddNode(node, NodeFactory->CreateImmutableNode(PatternNodes->HolderFactory->GetEmptyContainer()));
    }

    void Visit(TDataLiteral& node) override {
        auto value = node.AsValue();
        NUdf::TDataTypeId typeId = node.GetType()->GetSchemeType();
        if (typeId != 0x101) { // TODO remove
            const auto slot = NUdf::GetDataSlot(typeId);
            MKQL_ENSURE(IsValidValue(slot, value),
                "Bad data literal for type: " << NUdf::GetDataTypeInfo(slot).Name << ", " << value);
        }

        NUdf::TUnboxedValue externalValue;
        if (ExternalAlloc) {
            if (value.IsString()) {
                externalValue = MakeString(value.AsStringRef());
            }
        }
        if (!externalValue) {
            externalValue = std::move(value);
        }

        AddNode(node, NodeFactory->CreateImmutableNode(std::move(externalValue)));
    }

    void Visit(TStructLiteral& node) override {
        TComputationNodePtrVector values;
        values.reserve(node.GetValuesCount());
        for (ui32 i = 0, e = node.GetValuesCount(); i < e; ++i) {
            values.push_back(GetComputationNode(node.GetValue(i).GetNode()));
        }

        AddNode(node, NodeFactory->CreateArrayNode(std::move(values)));
    }

    void Visit(TListLiteral& node) override {
        TComputationNodePtrVector items;
        items.reserve(node.GetItemsCount());
        for (ui32 i = 0; i < node.GetItemsCount(); ++i) {
            items.push_back(GetComputationNode(node.GetItems()[i].GetNode()));
        }

        AddNode(node, NodeFactory->CreateArrayNode(std::move(items)));
    }

    void Visit(TOptionalLiteral& node) override {
        auto item = node.HasItem() ? GetComputationNode(node.GetItem().GetNode()) : nullptr;
        AddNode(node, NodeFactory->CreateOptionalNode(item));
    }

    void Visit(TDictLiteral& node) override {
        auto keyType = node.GetType()->GetKeyType();
        TKeyTypes types;
        bool isTuple;
        bool encoded;
        bool useIHash;
        GetDictionaryKeyTypes(keyType, types, isTuple, encoded, useIHash);

        std::vector<std::pair<IComputationNode*, IComputationNode*>> items;
        items.reserve(node.GetItemsCount());
        for (ui32 i = 0, e = node.GetItemsCount(); i < e; ++i) {
            auto item = node.GetItem(i);
            items.push_back(std::make_pair(GetComputationNode(item.first.GetNode()), GetComputationNode(item.second.GetNode())));
        }

        AddNode(node, NodeFactory->CreateDictNode(std::move(items), types, isTuple, encoded ? keyType : nullptr,
            useIHash ? MakeHashImpl(keyType) : nullptr, useIHash ? MakeEquateImpl(keyType) : nullptr));
    }

    void Visit(TCallable& node) override {
        if (node.HasResult()) {
            node.GetResult().GetNode()->Accept(*this);
            auto computationNode = PatternNodes->ComputationNodesList.back().Get();
            node.SetCookie((ui64)computationNode);
            return;
        }

        if (node.GetType()->GetName() == "Steal") {
            return;
        }

        TNodeLocator nodeLocator = [this](TNode* dependentNode, bool pop) {
            return GetComputationNode(dependentNode, pop);
        };
        TComputationNodeFactoryContext ctx(
                nodeLocator,
                FunctionRegistry,
                Env,
                TypeInfoHelper,
                CountersProvider,
                SecureParamsProvider,
                *NodeFactory,
                *PatternNodes->HolderFactory,
                PatternNodes->ValueBuilder.Get(),
                ValidateMode,
                ValidatePolicy,
                GraphPerProcess,
                PatternNodes->Mutables,
                PatternNodes->ElementsCache,
                std::bind(&TComputationGraphBuildingVisitor::PushBackNode, this, std::placeholders::_1));
        const auto computationNode = Factory(node, ctx);

        if (!computationNode) {
            THROW yexception()
                << "Computation graph builder, unsupported function: " << node.GetType()->GetName() << " type: " << Factory.target_type().name() ;
        }

        AddNode(node, computationNode);
    }

    void Visit(TAny& node) override {
        if (!node.HasItem()) {
            AddNode(node, NodeFactory->CreateImmutableNode(NUdf::TUnboxedValue::Void()));
        } else {
            AddNode(node, GetComputationNode(node.GetItem().GetNode()));
        }
    }

    void Visit(TTupleLiteral& node) override {
        TComputationNodePtrVector values;
        values.reserve(node.GetValuesCount());
        for (ui32 i = 0, e = node.GetValuesCount(); i < e; ++i) {
            values.push_back(GetComputationNode(node.GetValue(i).GetNode()));
        }

        AddNode(node, NodeFactory->CreateArrayNode(std::move(values)));
    }

    void Visit(TVariantLiteral& node) override {
        auto item = GetComputationNode(node.GetItem().GetNode());
        AddNode(node, NodeFactory->CreateVariantNode(item, node.GetIndex()));
    }

public:
    IComputationNode* GetComputationNode(TNode* node, bool pop = false, bool require = true) {
        return PatternNodes->GetComputationNode(node, pop, require);
    }

    TMemoryUsageInfo& GetMemInfo() {
        return *PatternNodes->MemInfo;
    }

    const THolderFactory& GetHolderFactory() const {
        return *PatternNodes->HolderFactory;
    }

    TPatternNodes::TPtr GetPatternNodes() {
        return PatternNodes;
    }

    const TComputationNodePtrDeque& GetNodes() const {
        return PatternNodes->GetNodes();
    }

    void PreserveRoot(IComputationNode* rootNode) {
        PatternNodes->RootNode = rootNode;
    }

    void PreserveEntryPoints(TComputationExternalNodePtrVector&& runtime2Computation) {
        PatternNodes->Runtime2Computation = std::move(runtime2Computation);
    }

private:
    void PushBackNode(const IComputationNode::TPtr& computationNode) {
        computationNode->RegisterDependencies();
        PatternNodes->ComputationNodesList.push_back(computationNode);
    }

    void AddNode(TNode& node, const IComputationNode::TPtr& computationNode) {
        PushBackNode(computationNode);
        node.SetCookie((ui64)computationNode.Get());
    }

private:
    const TTypeEnvironment& Env;
    NUdf::ITypeInfoHelper::TPtr TypeInfoHelper;
    NUdf::ICountersProvider* CountersProvider;
    const NUdf::ISecureParamsProvider* SecureParamsProvider;
    const TComputationNodeFactory Factory;
    const IFunctionRegistry& FunctionRegistry;
    TIntrusivePtr<TMemoryUsageInfo> MemInfo;
    THolder<TNodeFactory> NodeFactory;
    NUdf::EValidateMode ValidateMode;
    NUdf::EValidatePolicy ValidatePolicy;
    EGraphPerProcess GraphPerProcess;
    TPatternNodes::TPtr PatternNodes;
    const bool ExternalAlloc;
};

class TComputationGraph : public IComputationGraph {
public:
    TComputationGraph(TPatternNodes::TPtr& patternNodes, const TComputationOptsFull& compOpts)
        : PatternNodes(patternNodes)
        , MemInfo(MakeIntrusive<TMemoryUsageInfo>("ComputationGraph"))
        , CompOpts(compOpts)
    {
#ifndef NDEBUG
        CompOpts.AllocState.ActiveMemInfo.emplace(MemInfo.Get(), MemInfo);
#endif
        HolderFactory = MakeHolder<THolderFactory>(CompOpts.AllocState, *MemInfo, patternNodes->HolderFactory->GetFunctionRegistry());
        ValueBuilder = MakeHolder<TDefaultValueBuilder>(*HolderFactory.Get(), compOpts.ValidatePolicy);
        ValueBuilder->SetSecureParamsProvider(CompOpts.SecureParamsProvider);
        ArrowMemoryPool = MakeArrowMemoryPool(CompOpts.AllocState);
    }

    ~TComputationGraph() {
        auto stats = CompOpts.Stats;
        auto& pagePool = HolderFactory->GetPagePool();
        MKQL_SET_MAX_STAT(stats, PagePool_PeakAllocated, pagePool.GetPeakAllocated());
        MKQL_SET_MAX_STAT(stats, PagePool_PeakUsed, pagePool.GetPeakUsed());
        MKQL_ADD_STAT(stats, PagePool_AllocCount, pagePool.GetAllocCount());
        MKQL_ADD_STAT(stats, PagePool_PageAllocCount, pagePool.GetPageAllocCount());
        MKQL_ADD_STAT(stats, PagePool_PageHitCount, pagePool.GetPageHitCount());
        MKQL_ADD_STAT(stats, PagePool_PageMissCount, pagePool.GetPageMissCount());
        MKQL_ADD_STAT(stats, PagePool_OffloadedAllocCount, pagePool.GetOffloadedAllocCount());
        MKQL_ADD_STAT(stats, PagePool_OffloadedBytes, pagePool.GetOffloadedBytes());
    }

    void Prepare() override {
        if (!IsPrepared) {
            Ctx.Reset(new TComputationContext(*HolderFactory,
                ValueBuilder.Get(),
                CompOpts,
                PatternNodes->GetMutables(),
                *ArrowMemoryPool));
            ValueBuilder->SetCalleePositionHolder(Ctx->CalleePosition);
            for (auto& node : PatternNodes->GetNodes()) {
                node->InitNode(*Ctx);
            }
            IsPrepared = true;
        }
    }

    TComputationContext& GetContext() override {
        Prepare();
        return *Ctx;
    }

    NUdf::TUnboxedValue GetValue() override {
        Prepare();
        return PatternNodes->GetRoot()->GetValue(*Ctx);
    }

    IComputationExternalNode* GetEntryPoint(size_t index, bool require) override {
        Prepare();
        return PatternNodes->GetEntryPoint(index, require);
    }

    void Invalidate() override {
        std::fill_n(Ctx->MutableValues.get(), PatternNodes->GetMutables().CurValueIndex, NUdf::TUnboxedValue(NUdf::TUnboxedValuePod::Invalid()));
    }

    const TComputationNodePtrDeque& GetNodes() const override {
        return PatternNodes->GetNodes();
    }

    TMemoryUsageInfo& GetMemInfo() const override {
        return *MemInfo;
    }

    const THolderFactory& GetHolderFactory() const override {
        return *HolderFactory;
    }

    ITerminator* GetTerminator() const override {
        return ValueBuilder.Get();
    }

    bool SetExecuteLLVM(bool value) override {
        const bool old = Ctx->ExecuteLLVM;
        Ctx->ExecuteLLVM = value;
        return old;
    }

    TString SaveGraphState() override {
        Prepare();

        TString result;
        for (ui32 i : PatternNodes->GetMutables().SerializableValues) {
            const NUdf::TUnboxedValuePod& mutableValue = Ctx->MutableValues[i];
            if (mutableValue.IsInvalid()) {
                WriteUi32(result, std::numeric_limits<ui32>::max()); // -1.
            } else if (mutableValue.IsBoxed()) {
                NUdf::TUnboxedValue saved = mutableValue.Save();
                const TStringBuf savedBuf = saved.AsStringRef();
                WriteUi32(result, savedBuf.Size());
                result.AppendNoAlias(savedBuf.Data(), savedBuf.Size());
            } else { // No load was done during previous runs (if any).
                MKQL_ENSURE(mutableValue.HasValue() && (mutableValue.IsString() || mutableValue.IsEmbedded()), "State is expected to have data or invalid value");
                const NUdf::TStringRef savedRef = mutableValue.AsStringRef();
                WriteUi32(result, savedRef.Size());
                result.AppendNoAlias(savedRef.Data(), savedRef.Size());
            }
        }
        return result;
    }

    void LoadGraphState(TStringBuf state) override {
        Prepare();

        for (ui32 i : PatternNodes->GetMutables().SerializableValues) {
            if (const ui32 size = ReadUi32(state); size != std::numeric_limits<ui32>::max()) {
                MKQL_ENSURE(state.Size() >= size, "Serialized state is corrupted - buffer is too short (" << state.Size() << ") for specified size: " << size);
                const NUdf::TStringRef savedRef(state.Data(), size);
                Ctx->MutableValues[i] = MakeString(savedRef);
                state.Skip(size);
            } // else leave it Invalid()
        }

        MKQL_ENSURE(state.Empty(), "Serialized state is corrupted - extra bytes left: " << state.Size());
    }

private:
    const TPatternNodes::TPtr PatternNodes;
    const TIntrusivePtr<TMemoryUsageInfo> MemInfo;
    THolder<THolderFactory> HolderFactory;
    THolder<TDefaultValueBuilder> ValueBuilder;
    std::unique_ptr<arrow::MemoryPool> ArrowMemoryPool;
    THolder<TComputationContext> Ctx;
    TComputationOptsFull CompOpts;
    bool IsPrepared = false;
};

} // namespace

class TComputationPatternImpl : public IComputationPattern {
public:
    TComputationPatternImpl(THolder<TComputationGraphBuildingVisitor>&& builder, const TComputationPatternOpts& opts)
#if defined(MKQL_DISABLE_CODEGEN)
        : Codegen()
#elif defined(MKQL_FORCE_USE_CODEGEN)
        : Codegen(NYql::NCodegen::ICodegen::Make(NYql::NCodegen::ETarget::Native))
#else
        : Codegen(opts.OptLLVM != "OFF" || GetEnv(TString("MKQL_FORCE_USE_LLVM")) ? NYql::NCodegen::ICodegen::Make(NYql::NCodegen::ETarget::Native) : NYql::NCodegen::ICodegen::TPtr())
#endif
    {
        const auto& nodes = builder->GetNodes();
        for (const auto& node : nodes)
            node->PrepareStageOne();
        for (const auto& node : nodes)
            node->PrepareStageTwo();
        MKQL_ADD_STAT(opts.Stats, Mkql_TotalNodes, nodes.size());
#ifndef MKQL_DISABLE_CODEGEN
        if (Codegen) {
            TStatTimer timerFull(CodeGen_FullTime);
            timerFull.Acquire();
            bool hasCode = false;
            {
                TStatTimer timerGen(CodeGen_GenerateTime);
                timerGen.Acquire();
                for (auto it = nodes.crbegin(); nodes.crend() != it; ++it) {
                    if (const auto codegen = dynamic_cast<ICodegeneratorRootNode*>(it->Get())) {
                        try {
                            codegen->GenerateFunctions(Codegen);
                            hasCode = true;
                        } catch (const TNoCodegen&) {
                            hasCode = false;
                            break;
                        }
                    }
                }
                timerGen.Release();
                timerGen.Report(opts.Stats);
            }

            if (hasCode) {
                if (opts.OptLLVM.Contains("--dump-generated")) {
                    Cerr << "############### Begin generated module ###############" << Endl;
                    Codegen->GetModule().print(llvm::errs(), nullptr);
                    Cerr << "################ End generated module ################" << Endl;
                }

                TStatTimer timerComp(CodeGen_CompileTime);
                timerComp.Acquire();

                NYql::NCodegen::TCodegenStats codegenStats;
                Codegen->GetStats(codegenStats);
                MKQL_ADD_STAT(opts.Stats, CodeGen_TotalFunctions, codegenStats.TotalFunctions);
                MKQL_ADD_STAT(opts.Stats, CodeGen_TotalInstructions, codegenStats.TotalInstructions);
                MKQL_SET_MAX_STAT(opts.Stats, CodeGen_MaxFunctionInstructions, codegenStats.MaxFunctionInstructions);
                if (opts.OptLLVM.Contains("--dump-stats")) {
                    Cerr << "TotalFunctions: " << codegenStats.TotalFunctions << Endl;
                    Cerr << "TotalInstructions: " << codegenStats.TotalInstructions << Endl;
                    Cerr << "MaxFunctionInstructions: " << codegenStats.MaxFunctionInstructions << Endl;
                }

                if (opts.OptLLVM.Contains("--dump-perf-map")) {
                    Codegen->TogglePerfJITEventListener();
                }

                if (codegenStats.TotalFunctions >= TotalFunctionsLimit ||
                    codegenStats.TotalInstructions >= TotalInstructionsLimit ||
                    codegenStats.MaxFunctionInstructions >= MaxFunctionInstructionsLimit) {
                    Codegen.reset();
                } else {
                    Codegen->Verify();
                    NYql::NCodegen::TCompileStats compileStats;
                    Codegen->Compile(GetCompileOptions(opts.OptLLVM), &compileStats);
                    MKQL_ADD_STAT(opts.Stats, CodeGen_ModulePassTime, compileStats.ModulePassTime);
                    MKQL_ADD_STAT(opts.Stats, CodeGen_FinalizeTime, compileStats.FinalizeTime);
                }

                timerComp.Release();
                timerComp.Report(opts.Stats);

                if (Codegen) {
                    if (opts.OptLLVM.Contains("--dump-compiled")) {
                        Cerr << "############### Begin compiled module ###############" << Endl;
                        Codegen->GetModule().print(llvm::errs(), nullptr);
                        Cerr << "################ End compiled module ################" << Endl;
                    }

                    if (opts.OptLLVM.Contains("--asm-compiled")) {
                        Cerr << "############### Begin compiled asm ###############" << Endl;
                        Codegen->ShowGeneratedFunctions(&Cerr);
                        Cerr << "################ End compiled asm ################" << Endl;
                    }

                    auto count = 0U;
                    for (const auto& node : nodes) {
                        if (const auto codegen = dynamic_cast<ICodegeneratorRootNode*>(node.Get())) {
                            codegen->FinalizeFunctions(Codegen);
                            ++count;
                        }
                    }

                    if (count)
                        MKQL_ADD_STAT(opts.Stats, Mkql_CodegenFunctions, count);
                }
            }

            timerFull.Release();
            timerFull.Report(opts.Stats);
        }
#endif
        PatternNodes = builder->GetPatternNodes();
    }

    ~TComputationPatternImpl() {
        if (TypeEnv) {
            auto guard = TypeEnv->BindAllocator();
            PatternNodes.Reset();
        }
    }

    void SetTypeEnv(TTypeEnvironment* typeEnv) {
        TypeEnv = typeEnv;
    }

    TStringBuf GetCompileOptions(const TString& s) {
        const TString flag = "--compile-options";
        auto lpos = s.rfind(flag);
        if (lpos == TString::npos)
            return TStringBuf();
        lpos += flag.Size();
        auto rpos = s.find(" --", lpos);
        if (rpos == TString::npos)
            return TStringBuf(s, lpos);
        else
            return TStringBuf(s, lpos, rpos - lpos);
    };

    THolder<IComputationGraph> Clone(const TComputationOptsFull& compOpts) final {
        return MakeHolder<TComputationGraph>(PatternNodes, compOpts);
    }

private:
    TTypeEnvironment* TypeEnv = nullptr;
    TPatternNodes::TPtr PatternNodes;
    NYql::NCodegen::ICodegen::TPtr Codegen;
};


TIntrusivePtr<TComputationPatternImpl> MakeComputationPatternImpl(TExploringNodeVisitor& explorer, const TRuntimeNode& root,
        const std::vector<TNode*>& entryPoints, const TComputationPatternOpts& opts) {
    TDependencyScanVisitor depScanner;
    depScanner.Walk(root.GetNode(), opts.Env);

    auto builder = MakeHolder<TComputationGraphBuildingVisitor>(opts);
    for (const auto& node : explorer.GetNodes()) {
        Y_VERIFY(node->GetCookie() <= IS_NODE_REACHABLE, "TNode graph should not be reused");
        if (node->GetCookie() == IS_NODE_REACHABLE) {
            node->Accept(*builder);
        }
    }

    const auto rootNode = builder->GetComputationNode(root.GetNode());

    TComputationExternalNodePtrVector runtime2Computation;
    runtime2Computation.resize(entryPoints.size(), nullptr);
    for (const auto& node : explorer.GetNodes()) {
        for (auto iter = std::find(entryPoints.cbegin(), entryPoints.cend(), node); entryPoints.cend() != iter; iter = std::find(iter + 1, entryPoints.cend(), node)) {
            runtime2Computation[iter - entryPoints.begin()] = dynamic_cast<IComputationExternalNode*>(builder->GetComputationNode(node));
        }
        node->SetCookie(0);
    }
    builder->PreserveRoot(rootNode);
    builder->PreserveEntryPoints(std::move(runtime2Computation));

    return MakeIntrusive<TComputationPatternImpl>(std::move(builder), opts);
}

IComputationPattern::TPtr MakeComputationPattern(TExploringNodeVisitor& explorer, const TRuntimeNode& root,
        const std::vector<TNode*>& entryPoints, const TComputationPatternOpts& opts) {
    auto pattern = MakeComputationPatternImpl(explorer, root, entryPoints, opts);
    if (opts.PatternEnv) {
        pattern->SetTypeEnv(&opts.PatternEnv->Env);
    }
    return pattern;
}

class TComputationPatternCache: public IComputationPatternCache {
public:
    IComputationPattern::TPtr EmplacePattern(const TString& serialized, PrepareFunc prepareFunc) override;
    void CleanCache() override {
        RewriteToCache.clear();
    }
    size_t GetSize() const override {
        return RewriteToCache.size();
    }
    size_t GetCacheHits() const override {
        return CacheHits;
    }
private:
    TMutex CacheMutex;
    THashMap<uint128, IComputationPattern::TPtr> RewriteToCache;
    ui64 CacheHits = 0;
    ui64 CacheMiss = 0;
};

IComputationPatternCache::TPtr IComputationPatternCache::Create() {
    return THolder(new TComputationPatternCache());
}

IComputationPattern::TPtr TComputationPatternCache::EmplacePattern(const TString& serialized, PrepareFunc prepareFunc) {
    const uint128 hash = CityHash128(serialized);
    with_lock(CacheMutex) {
        auto iter = RewriteToCache.find(hash);
        if (iter == RewriteToCache.end()) {
            ++CacheMiss;
            // TODO: do not block without collision by prepareFunc()
            iter = RewriteToCache.emplace(hash, prepareFunc()).first;
        } else {
            ++CacheHits;
        }
        return iter->second;
    }
}

} // namespace NMiniKQL
} // namespace NKikimr

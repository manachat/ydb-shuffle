#include "dq_output_consumer.h"

#include <ydb/library/yql/dq/actors/protos/dq_events.pb.h>
#include <yql/essentials/minikql/computation/mkql_block_builder.h>
#include <yql/essentials/minikql/computation/mkql_block_reader.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/mkql_node.h>
#include <yql/essentials/minikql/mkql_type_builder.h>

#include <yql/essentials/public/udf/arrow/args_dechunker.h>
#include <yql/essentials/public/udf/arrow/memory_pool.h>
#include <yql/essentials/public/udf/udf_value.h>

#include <yql/essentials/utils/yql_panic.h>

#define coordinator_v0

#define shuf_logs

#ifdef shuf_logs

#include <yql/essentials/utils/log/log.h>


#endif

#ifdef coordinator_v0

#include <mutex>
#include <util/generic/vector.h>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <functional>
#include <utility>

#include <ydb/library/formats/arrow/size_calcer.h>

#endif

namespace NYql::NDq {

namespace {

using namespace NKikimr;
using namespace NMiniKQL;
using namespace NUdf;


#ifdef coordinator_v0

static const ui64 SAMPLE_BUFFER_SIZE = 1000; //10L * 1024 * 1024 * 1024;

static std::atomic_int32_t PARTITION_SOURCE = 0;


class Coordinator;

class PartitionHandle {
public:

    PartitionHandle(ui32 num) = default;
    PartitionHandle(const PartitionHandle&) = default;
    PartitionHandle(PartitionHandle&&) = default;

    PartitionHandle(ui32 num, std::function<void()>&& callback)
    : HandleNumber_(num)
    , Count_(0)
    , CurrentBufferSize_(0)
    , SampleBuffer_()
    , Accept_(true)
    , FinishCallback_(std::move(callback)) 
    , PartitionFunction_([](const TUnboxedValue&) { return 0; })
    , PartitionerProvided_(false)
    { } 

    bool accept(TUnboxedValue&& value) {
        YQL_ENSURE(Accept_);
        ui64 size = estimateSize(value);
        if (this->CurrentBufferSize_ + size > SAMPLE_BUFFER_SIZE) {
            YQL_LOG(INFO) << "Handle " << HandleNumber_ << " finished";
            Accept_ = false;
            FinishCallback_();
            return false;
        }
        Count_ += 1;
        CurrentBufferSize_ += size;
        SampleBuffer_.emplace_back(std::move(value));
        return true;
    }

    const TVector<TUnboxedValue>& buffer() const {
        return SampleBuffer_;
    }

    size_t Partition(const TUnboxedValue& value) const {
        return PartitionFunction_(value);
    }

    bool HasPartitionFunction() const {
        return PartitionerProvided_.load();
    }

    void ProvidePartitionFunction(std::function<size_t(const TUnboxedValue&)>&& func) {
        PartitionFunction_ = std::move(func);
        PartitionerProvided_.store(true);
    }

    ui32 GetNumber() const {
        return HandleNumber_;
    }
    

private:
    ui64 estimateSize(const TUnboxedValue& value) const {
        // size_calcer ?
        // пока просто количество
        (void) value;
        return 1;
    }

private:
    ui32 HandleNumber_;
    ui64 Count_;
    ui64 CurrentBufferSize_;
    TVector<TUnboxedValue> SampleBuffer_;
    bool Accept_;
    std::function<void()> FinishCallback_;
    std::function<size_t(const TUnboxedValue&)> PartitionFunction_;
    std::atomic_bool PartitionerProvided_;
};

class Coordinator {

private:
    enum state {
        collect_samples,
        sampled_execution,  
        finished
    };


  

    void calculateHist() {
        // типа считаем буферы со всех партиций
        for (auto& handle : Partitions_) {
            handle.second.ProvidePartitionFunction([] (const TUnboxedValue&) { return 0; });
        }
        for (auto& handle : Partitions_) {
            handle.second.ProvidePartitionFunction([] (const TUnboxedValue&) { return 0; });
        }
        State_.store(finished);
    }
    
public:

    Coordinator(ui32 partitions_count) 
    : PartitionsCount_(partitions_count)
    , Mutex_()
    , Partitions_(), RegisteredCount_(0)
    , State_(collect_samples)
    , CoordinatorCv_(), FinishedPartitions_(0)
    , PartitionsCv_() 
    {
    }

    Coordinator(Coordinator&&) = default;

    Coordinator& operator=(Coordinator&&) = default;

    PartitionHandle& registerPartition() {
        int32_t id = PARTITION_SOURCE.fetch_add(1);
        return registerPartition(id);
    }

    PartitionHandle& registerPartition(ui32 num) {
        std::scoped_lock<std::mutex> lock(Mutex_);
        if (!Partitions_.contains(num)) {
          PartitionHandle handle(num, [this, num] {
            // синхронизируемся после сбора семпла
            std::unique_lock<std::mutex> lock(this->Mutex_);
            uint32_t parts = this->FinishedPartitions_.fetch_add(1);
            YQL_LOG(INFO) << "Partition " << num << " synchronizing, partition count: " << parts + 1;
            if (parts == PartitionsCount_ - 1) {
                // последний считает гистограмму и будит всех
                YQL_LOG(INFO) << "Partition " << num << " is the last one";
                this->calculateHist();
                PartitionsCv_.notify_all();
            } else {
                YQL_LOG(INFO) << "Partition " << num << " awaits others";
                this->PartitionsCv_.wait(lock, [this]() { return State_.load() == finished; });
            }
          });
          Partitions_.emplace(num, handle);
          RegisteredCount_.fetch_add(1); // idk, не используется
        }

        return this->Partitions_.at(num);
    }
    

private:
    const ui32 PartitionsCount_;
    std::mutex Mutex_;
    std::unordered_map<ui32, PartitionHandle> Partitions_;
    std::atomic_uint32_t RegisteredCount_;
    std::atomic<state> State_;

    // collect samples
    std::condition_variable CoordinatorCv_;
    std::atomic_int32_t FinishedPartitions_;

    // partition
    std::condition_variable PartitionsCv_;

};


static std::unordered_map<ui32, Coordinator> coordinators_;
static std::mutex coordinators_mutex_;

// q3 PRAGMA dq.MaxTasksPerStage='4';
static const ui32 PARTITIONS_COUNT_4 = 4;

static Coordinator& get_coordinator_for_stage(ui32 src) {
    std::scoped_lock<std::mutex> lock(coordinators_mutex_);
    if (!coordinators_.contains(src)) {
        coordinators_.emplace(src, Coordinator(PARTITIONS_COUNT_4));
    }
    return coordinators_.at(src);
}

#endif



class TDqOutputMultiConsumer : public IDqOutputConsumer {
public:
    explicit TDqOutputMultiConsumer(TVector<IDqOutputConsumer::TPtr>&& consumers)
        : Consumers(std::move(consumers))
    {
        YQL_ENSURE(!Consumers.empty());
    }

    bool IsFull() const override {
        return AnyOf(Consumers, [](const auto& consumer) { return consumer->IsFull(); });
    }

    void WideConsume(TUnboxedValue* values, ui32 count) override {
        Y_UNUSED(values);
        Y_UNUSED(count);
        YQL_ENSURE(false, "WideConsume is not supported");
    }

    void Consume(TUnboxedValue&& value) override {
        if (Consumers.size() == 1) {
            Consumers[0]->Consume(std::move(value));
            return;
        }

        auto index = value.GetVariantIndex();
        YQL_ENSURE(index < Consumers.size());
        auto variantItem = value.GetVariantItem();
        Consumers[index]->Consume(std::move(variantItem));
    }

    void Consume(NDqProto::TCheckpoint&& checkpoint) override {
        for (auto& consumer : Consumers) {
            consumer->Consume(NDqProto::TCheckpoint(checkpoint));
        }
    }

    void Finish() override {
        for (auto& consumer : Consumers) {
            consumer->Finish();
        }
    }

private:
    TVector<IDqOutputConsumer::TPtr> Consumers;
};

class TDqOutputMapConsumer : public IDqOutputConsumer {
public:
    TDqOutputMapConsumer(IDqOutput::TPtr output)
        : Output(output) {}

    bool IsFull() const override {
        return Output->IsFull();
    }

    void Consume(TUnboxedValue&& value) override {
        Output->Push(std::move(value));
    }

    void WideConsume(TUnboxedValue* values, ui32 count) override {
        Output->WidePush(values, count);
    }

    void Consume(NDqProto::TCheckpoint&& checkpoint) override {
        Output->Push(std::move(checkpoint));
    }

    void Finish() override {
        Output->Finish();
    }

private:
    IDqOutput::TPtr Output;
};

#ifdef coordinator_v0

class TDqHistPartitionConsumer : public IDqOutputConsumer {
private:
    mutable bool IsWaitingFlag = false;
    mutable TUnboxedValue WaitingValue;
    mutable TUnboxedValueVector WideWaitingValues;
    mutable IDqOutput::TPtr OutputWaiting;

protected:

    bool DrainHandleBuffer() const {
        if (!Handle_.HasPartitionFunction()) {
            return false;
        }
        // todo нормально забирать вектор
        for (auto el : Handle_.buffer()) {
            // может ждать?
            ui32 partitionIndex = GetHistPartitionIndex(el);
            Outputs[partitionIndex] -> Push(std::move(el));
        }
        
        return true;
    }

    void DrainWaiting() const {
        if (!DrainHandleBuffer()) {
            return;
        }
        if (Y_UNLIKELY(IsWaitingFlag)) {
            if (OutputWaiting->IsFull()) {
                return;
            }
            if (OutputWidth.Defined()) {
                YQL_ENSURE(OutputWidth == WideWaitingValues.size());
                OutputWaiting->WidePush(WideWaitingValues.data(), *OutputWidth);
            } else {
                OutputWaiting->Push(std::move(WaitingValue));
            }
            IsWaitingFlag = false;
        }
    }


    virtual bool DoTryFinish() override {
        DrainWaiting();
        return !IsWaitingFlag;
    }

public:

    TDqHistPartitionConsumer(ui32 srcStage, TVector<IDqOutput::TPtr>&& outputs, TVector<TColumnInfo>&& keyColumns, TMaybe<ui32> outputWidth) 
    : Outputs(std::move(outputs))
    , KeyColumns(std::move(keyColumns))
    , OutputWidth(outputWidth)
    , Handle_(get_coordinator_for_stage(srcStage).registerPartition())
    , CollectSample_(true) 
    , StageNum_(srcStage)
    {
        YQL_LOG(INFO) << "Hist consumer for stage " << srcStage;
    }

    ~TDqHistPartitionConsumer() {
        YQL_LOG(INFO) << "Partitioner " << Handle_.GetNumber() << " of stage " << StageNum_ << " destroyed";
    }

    bool IsFull() const override {
        DrainWaiting();
        return IsWaitingFlag;
    }

    void Consume(NKikimr::NUdf::TUnboxedValue &&value) override {
        YQL_ENSURE(!OutputWidth.Defined());

        if (CollectSample_) {
            if (!Handle_.accept(std::move(value))) {
                YQL_LOG(INFO) << "Partitioner " << Handle_.GetNumber() << " of stage " << StageNum_ << " finished sampling";
                CollectSample_ = false;
                IsWaitingFlag = true;   
            }
            return;
        }

        ui32 partitionIndex = GetHistPartitionIndex(value);
        if (Outputs[partitionIndex]->IsFull()) {
            YQL_ENSURE(!IsWaitingFlag);
            IsWaitingFlag = true;
            OutputWaiting = Outputs[partitionIndex];
            WaitingValue = std::move(value);
        } else {
            Outputs[partitionIndex]->Push(std::move(value));
        }

    }

    void WideConsume(NKikimr::NUdf::TUnboxedValue *values, ui32 count) override {
        YQL_ENSURE(OutputWidth.Defined() && count == OutputWidth);
        ui32 partitionIndex = GetHistPartitionIndex(values);
        if (Outputs[partitionIndex]->IsFull()) {
            YQL_ENSURE(!IsWaitingFlag);
            IsWaitingFlag = true;
            OutputWaiting = Outputs[partitionIndex];
            std::move(values, values + count, WideWaitingValues.data());
        } else {
            Outputs[partitionIndex]->WidePush(values, count);
        }
    }

    void Consume(NDqProto::TCheckpoint &&checkpoint) override {
        for (auto& output : Outputs) {
            output->Push(NDqProto::TCheckpoint(checkpoint));
        }
    }

    void Finish() override {
        for (auto& output : Outputs) {
            output->Finish();
        }
    }

private:

    size_t GetHistPartitionIndex(const TUnboxedValue& value) const {
        YQL_ENSURE(Handle_.HasPartitionFunction());
        return Handle_.Partition(value);
    }

    size_t GetHistPartitionIndex(const TUnboxedValue*) const {
        // todo
        YQL_ENSURE(false, "unimplemented");
        return 0;
    }


private:
    const TVector<IDqOutput::TPtr> Outputs;
    const TVector<TColumnInfo> KeyColumns;
    const TMaybe<ui32> OutputWidth;
    PartitionHandle& Handle_;
    bool CollectSample_;
    ui32 StageNum_;
    
};

#endif

class TDqOutputHashPartitionConsumer : public IDqOutputConsumer {
private:
    mutable bool IsWaitingFlag = false;
    mutable TUnboxedValue WaitingValue;
    mutable TUnboxedValueVector WideWaitingValues;
    mutable IDqOutput::TPtr OutputWaiting;
protected:
    void DrainWaiting() const {
        if (Y_UNLIKELY(IsWaitingFlag)) {
            if (OutputWaiting->IsFull()) {
                return;
            }
            if (OutputWidth.Defined()) {
                YQL_ENSURE(OutputWidth == WideWaitingValues.size());
                OutputWaiting->WidePush(WideWaitingValues.data(), *OutputWidth);
            } else {
                OutputWaiting->Push(std::move(WaitingValue));
            }
            IsWaitingFlag = false;
        }
    }

    virtual bool DoTryFinish() override {
        DrainWaiting();
        return !IsWaitingFlag;
    }
public:
    TDqOutputHashPartitionConsumer(TVector<IDqOutput::TPtr>&& outputs, TVector<TColumnInfo>&& keyColumns, TMaybe<ui32> outputWidth)
        : Outputs(std::move(outputs))
        , KeyColumns(std::move(keyColumns))
        , OutputWidth(outputWidth)
    {
        for (auto& column : KeyColumns) {
            ValueHashers.emplace_back(MakeHashImpl(column.Type));
        }

        if (outputWidth.Defined()) {
            WideWaitingValues.resize(*outputWidth);
        }

        #ifdef shuf_logs
        YQL_LOG(INFO) << "Create TDqOutputHashPartitionConsumer";
        #endif
    }

    bool IsFull() const override {
        DrainWaiting();
        return IsWaitingFlag;
    }

    void Consume(TUnboxedValue&& value) final {
        YQL_ENSURE(!OutputWidth.Defined());
        ui32 partitionIndex = GetHashPartitionIndex(value);
        if (Outputs[partitionIndex]->IsFull()) {
            YQL_ENSURE(!IsWaitingFlag);
            IsWaitingFlag = true;
            OutputWaiting = Outputs[partitionIndex];
            WaitingValue = std::move(value);
        } else {
            Outputs[partitionIndex]->Push(std::move(value));
        }
    }

    void WideConsume(TUnboxedValue* values, ui32 count) final {
        YQL_ENSURE(OutputWidth.Defined() && count == OutputWidth);
        ui32 partitionIndex = GetHashPartitionIndex(values);
        if (Outputs[partitionIndex]->IsFull()) {
            YQL_ENSURE(!IsWaitingFlag);
            IsWaitingFlag = true;
            OutputWaiting = Outputs[partitionIndex];
            std::move(values, values + count, WideWaitingValues.data());
        } else {
            Outputs[partitionIndex]->WidePush(values, count);
        }
    }

    void Consume(NDqProto::TCheckpoint&& checkpoint) override {
        for (auto& output : Outputs) {
            output->Push(NDqProto::TCheckpoint(checkpoint));
        }
    }

    void Finish() final {
        for (auto& output : Outputs) {
            output->Finish();
        }
    }

private:
    size_t GetHashPartitionIndex(const TUnboxedValue& value) {
        ui64 hash = 0;

        for (size_t keyId = 0; keyId < KeyColumns.size(); keyId++) {
            auto columnValue = value.GetElement(KeyColumns[keyId].Index);
            hash = CombineHashes(hash, HashColumn(keyId, columnValue));
        }

        return hash % Outputs.size();
    }

    size_t GetHashPartitionIndex(const TUnboxedValue* values) {
        ui64 hash = 0;

        for (size_t keyId = 0; keyId < KeyColumns.size(); keyId++) {
            MKQL_ENSURE_S(KeyColumns[keyId].Index < OutputWidth);
            hash = CombineHashes(hash, HashColumn(keyId, values[KeyColumns[keyId].Index]));
        }

        return hash % Outputs.size();
    }

    ui64 HashColumn(size_t keyId, const TUnboxedValue& value) const {
        if (!value.HasValue()) {
            return 0;
        }
        return ValueHashers[keyId]->Hash(value);
    }

private:
    const TVector<IDqOutput::TPtr> Outputs;
    const TVector<TColumnInfo> KeyColumns;
    TVector<NUdf::IHash::TPtr> ValueHashers;
    const TMaybe<ui32> OutputWidth;
};

class TDqOutputHashPartitionConsumerScalar : public IDqOutputConsumer {
public:
    TDqOutputHashPartitionConsumerScalar(TVector<IDqOutput::TPtr>&& outputs, TVector<TColumnInfo>&& keyColumns, const  NKikimr::NMiniKQL::TType* outputType)
        : Outputs_(std::move(outputs))
        , KeyColumns_(std::move(keyColumns))
        , OutputWidth_(static_cast<const NMiniKQL::TMultiType*>(outputType)->GetElementsCount())
        , WaitingValues_(OutputWidth_)
    {
        auto multiType = static_cast<const NMiniKQL::TMultiType*>(outputType);
        TBlockTypeHelper helper;
        for (auto& column : KeyColumns_) {
            auto columnType = multiType->GetElementType(column.Index);
            YQL_ENSURE(columnType->IsBlock());
            auto blockType = static_cast<const NMiniKQL::TBlockType*>(columnType);
            YQL_ENSURE(blockType->GetShape() == NMiniKQL::TBlockType::EShape::Scalar);
            Readers_.emplace_back(MakeBlockReader(TTypeInfoHelper(), blockType->GetItemType()));
            Hashers_.emplace_back(helper.MakeHasher(blockType->GetItemType()));
        }
        #ifdef shuf_logs
        YQL_LOG(INFO) << "Create TDqOutputHashPartitionConsumerScalar";
        #endif
    }
private:
    bool IsFull() const final {
        DrainWaiting();
        return IsWaitingFlag_;
    }

    void Consume(TUnboxedValue&& value) final {
        Y_UNUSED(value);
        YQL_ENSURE(false, "Consume() called on wide block stream");
    }

    void WideConsume(TUnboxedValue* values, ui32 count) final {
        YQL_ENSURE(count == OutputWidth_);
        const ui64 inputBlockLen = TArrowBlock::From(values[count - 1]).GetDatum().scalar_as<arrow::UInt64Scalar>().value;
        if (!inputBlockLen) {
            return;
        }

        if (!Output_) {
            Output_ = Outputs_[GetHashPartitionIndex(values)];
        }
        if (Output_->IsFull()) {
            YQL_ENSURE(!IsWaitingFlag_);
            IsWaitingFlag_ = true;
            std::move(values, values + count, WaitingValues_.data());
        } else {
            Output_->WidePush(values, count);
        }
    }

    void Consume(NDqProto::TCheckpoint&& checkpoint) override {
        for (auto& output : Outputs_) {
            output->Push(NDqProto::TCheckpoint(checkpoint));
        }
    }

    void Finish() final {
        for (auto& output : Outputs_) {
            output->Finish();
        }
    }

    void DrainWaiting() const {
        if (Y_UNLIKELY(IsWaitingFlag_)) {
            YQL_ENSURE(Output_);
            if (Output_->IsFull()) {
                return;
            }
            YQL_ENSURE(OutputWidth_ == WaitingValues_.size());
            Output_->WidePush(WaitingValues_.data(), OutputWidth_);
            IsWaitingFlag_ = false;
        }
    }

    bool DoTryFinish() final {
        DrainWaiting();
        return !IsWaitingFlag_;
    }

    size_t GetHashPartitionIndex(const TUnboxedValue* values) {
        ui64 hash = 0;

        for (size_t keyId = 0; keyId < KeyColumns_.size(); keyId++) {
            YQL_ENSURE(KeyColumns_[keyId].Index < OutputWidth_);
            hash = CombineHashes(hash, HashColumn(keyId, values[KeyColumns_[keyId].Index]));
        }

        return hash % Outputs_.size();
    }

    ui64 HashColumn(size_t keyId, const TUnboxedValue& value) const {
        TBlockItem item = Readers_[keyId]->GetScalarItem(*TArrowBlock::From(value).GetDatum().scalar());
        return Hashers_[keyId]->Hash(item);
    }

private:
    const TVector<IDqOutput::TPtr> Outputs_;
    const TVector<TColumnInfo> KeyColumns_;
    const ui32 OutputWidth_;
    TVector<NUdf::IBlockItemHasher::TPtr> Hashers_;
    TVector<std::unique_ptr<IBlockReader>> Readers_;
    IDqOutput::TPtr Output_;
    mutable bool IsWaitingFlag_ = false;
    mutable TUnboxedValueVector WaitingValues_;
};

class TDqOutputHashPartitionConsumerBlock : public IDqOutputConsumer {
public:
    TDqOutputHashPartitionConsumerBlock(TVector<IDqOutput::TPtr>&& outputs, TVector<TColumnInfo>&& keyColumns,
        const  NKikimr::NMiniKQL::TType* outputType,
        const NKikimr::NMiniKQL::THolderFactory& holderFactory,
        TMaybe<ui8> minFillPercentage,
        NUdf::IPgBuilder* pgBuilder)
        : OutputType_(static_cast<const NMiniKQL::TMultiType*>(outputType))
        , HolderFactory_(holderFactory)
        , Outputs_(std::move(outputs))
        , KeyColumns_(std::move(keyColumns))
        , ScalarColumnHashes_(KeyColumns_.size())
        , OutputWidth_(OutputType_->GetElementsCount())
        , MinFillPercentage_(minFillPercentage)
        , PgBuilder_(pgBuilder)
    {
        TTypeInfoHelper helper;
        YQL_ENSURE(OutputWidth_ > KeyColumns_.size());

        TVector<const NMiniKQL::TType*> blockTypes;
        for (auto& columnType : OutputType_->GetElements()) {
            YQL_ENSURE(columnType->IsBlock());
            auto blockType = static_cast<const NMiniKQL::TBlockType*>(columnType);
            if (blockType->GetShape() == NMiniKQL::TBlockType::EShape::Many) {
                blockTypes.emplace_back(blockType->GetItemType());
            }
        }

        TBlockTypeHelper blockHelper;
        for (auto& column : KeyColumns_) {
            auto columnType = OutputType_->GetElementType(column.Index);
            YQL_ENSURE(columnType->IsBlock());
            auto blockType = static_cast<const NMiniKQL::TBlockType*>(columnType);
            Readers_.emplace_back(MakeBlockReader(helper, blockType->GetItemType()));
            Hashers_.emplace_back(blockHelper.MakeHasher(blockType->GetItemType()));
        }
        #ifdef shuf_logs
        YQL_LOG(INFO) << "Create TDqOutputHashPartitionConsumerBlock";
        #endif
    }

private:
    bool IsFull() const final {
        DrainWaiting();
        return IsWaitingFlag_;
    }

    void Consume(TUnboxedValue&& value) final {
        Y_UNUSED(value);
        YQL_ENSURE(false, "Consume() called on wide block stream");
    }

    void WideConsume(TUnboxedValue values[], ui32 count) final {
        YQL_ENSURE(!IsWaitingFlag_);
        YQL_ENSURE(count == OutputWidth_);

        const ui64 inputBlockLen = TArrowBlock::From(values[count - 1]).GetDatum().scalar_as<arrow::UInt64Scalar>().value;
        if (!inputBlockLen) {
            return;
        }

        TVector<const arrow::Datum*> datums;
        datums.reserve(count - 1);
        for (ui32 i = 0; i < count - 1; ++i) {
            datums.push_back(&TArrowBlock::From(values[i]).GetDatum());
        }

        TVector<TVector<ui64>> outputBlockIndexes(Outputs_.size());
        for (ui64 i = 0; i < inputBlockLen; ++i) {
            outputBlockIndexes[GetHashPartitionIndex(datums.data(), i)].push_back(i);
        }

        TVector<std::unique_ptr<TArgsDechunker>> outputData;
        for (size_t i = 0; i < Outputs_.size(); ++i) {
            ui64 outputBlockLen = outputBlockIndexes[i].size();
            if (!outputBlockLen) {
                outputData.emplace_back();
                continue;
            }
            MakeBuilders(outputBlockLen);
            const ui64* indexes = outputBlockIndexes[i].data();

            std::vector<arrow::Datum> output;
            for (size_t j = 0; j < datums.size(); ++j) {
                const arrow::Datum* src = datums[j];
                if (src->is_scalar()) {
                    output.emplace_back(*src);
                } else {
                    IArrayBuilder::TArrayDataItem dataItem {
                        .Data = src->array().get(),
                        .StartOffset = 0,
                    };
                    Builders_[j]->AddMany(&dataItem, 1, indexes, outputBlockLen);
                    output.emplace_back(Builders_[j]->Build(true));
                }
            }
            outputData.emplace_back(std::make_unique<TArgsDechunker>(std::move(output)));
        }

        DoConsume(std::move(outputData));
    }

    void DoConsume(TVector<std::unique_ptr<TArgsDechunker>>&& outputData) const {
        Y_ENSURE(outputData.size() == Outputs_.size());

        while (!outputData.empty()) {
            bool hasData = false;
            for (size_t i = 0; i < Outputs_.size(); ++i) {
                if (Outputs_[i]->IsFull()) {
                    IsWaitingFlag_ = true;
                    Y_ENSURE(OutputData_.empty());
                    OutputData_ = std::move(outputData);
                    return;
                }

                std::vector<arrow::Datum> chunk;
                ui64 blockLen = 0;
                if (outputData[i] && outputData[i]->Next(chunk, blockLen)) {
                    YQL_ENSURE(blockLen > 0);
                    hasData = true;
                    TUnboxedValueVector outputValues;
                    for (auto& datum : chunk) {
                        outputValues.emplace_back(HolderFactory_.CreateArrowBlock(std::move(datum)));
                    }
                    outputValues.emplace_back(HolderFactory_.CreateArrowBlock(arrow::Datum(std::make_shared<arrow::UInt64Scalar>(blockLen))));
                    Outputs_[i]->WidePush(outputValues.data(), outputValues.size());
                }
            }
            if (!hasData) {
                outputData.clear();
            }
        }
    }

    void Consume(NDqProto::TCheckpoint&& checkpoint) override {
        for (auto& output : Outputs_) {
            output->Push(NDqProto::TCheckpoint(checkpoint));
        }
    }

    void Finish() final {
        for (auto& output : Outputs_) {
            output->Finish();
        }
    }

    void DrainWaiting() const {
        if (Y_UNLIKELY(IsWaitingFlag_)) {
            TVector<std::unique_ptr<TArgsDechunker>> outputData;
            outputData.swap(OutputData_);
            DoConsume(std::move(outputData));
            if (OutputData_.empty()) {
                IsWaitingFlag_ = false;
            }
        }
    }

    bool DoTryFinish() final {
        DrainWaiting();
        return !IsWaitingFlag_;
    }

    size_t GetHashPartitionIndex(const arrow::Datum* values[], ui64 blockIndex) {
        ui64 hash = 0;
        for (size_t keyId = 0; keyId < KeyColumns_.size(); keyId++) {
            const ui32 columnIndex = KeyColumns_[keyId].Index;
            Y_DEBUG_ABORT_UNLESS(columnIndex < OutputWidth_);
            ui64 keyHash;
            if (*KeyColumns_[keyId].IsScalar) {
                if (!ScalarColumnHashes_[keyId].Defined()) {
                    ScalarColumnHashes_[keyId] = HashScalarColumn(keyId, *values[columnIndex]->scalar());
                }
                keyHash = *ScalarColumnHashes_[keyId];
            } else {
                keyHash = HashBlockColumn(keyId, *values[columnIndex]->array(), blockIndex);
            }
            hash = CombineHashes(hash, keyHash);
        }
        return hash % Outputs_.size();
    }

    inline ui64 HashScalarColumn(size_t keyId, const arrow::Scalar& value) const {
        TBlockItem item = Readers_[keyId]->GetScalarItem(value);
        return Hashers_[keyId]->Hash(item);
    }

    inline ui64 HashBlockColumn(size_t keyId, const arrow::ArrayData& value, ui64 index) const {
        TBlockItem item = Readers_[keyId]->GetItem(value, index);
        return Hashers_[keyId]->Hash(item);
    }

    void MakeBuilders(ui64 maxBlockLen) {
        Builders_.clear();
        TTypeInfoHelper helper;
        for (auto& columnType : OutputType_->GetElements()) {
            YQL_ENSURE(columnType->IsBlock());
            auto blockType = static_cast<const NMiniKQL::TBlockType*>(columnType);
            if (blockType->GetShape() == NMiniKQL::TBlockType::EShape::Many) {
                auto itemType = blockType->GetItemType();
                Builders_.emplace_back(MakeArrayBuilder(helper, itemType, *NYql::NUdf::GetYqlMemoryPool(), maxBlockLen, PgBuilder_, {.MinFillPercentage=MinFillPercentage_}));
            } else {
                Builders_.emplace_back();
            }
        }
    }

private:
    const NKikimr::NMiniKQL::TMultiType* const OutputType_;
    const NKikimr::NMiniKQL::THolderFactory& HolderFactory_;

    const TVector<IDqOutput::TPtr> Outputs_;
    mutable TVector<std::unique_ptr<TArgsDechunker>> OutputData_;

    const TVector<TColumnInfo> KeyColumns_;
    TVector<TMaybe<ui64>> ScalarColumnHashes_;
    const ui32 OutputWidth_;
    const TMaybe<ui8> MinFillPercentage_;

    TVector<NUdf::IBlockItemHasher::TPtr> Hashers_;
    TVector<std::unique_ptr<IBlockReader>> Readers_;
    TVector<std::unique_ptr<IArrayBuilder>> Builders_;

    mutable bool IsWaitingFlag_ = false;

    NUdf::IPgBuilder* PgBuilder_;
};

class TDqOutputBroadcastConsumer : public IDqOutputConsumer {
public:
    TDqOutputBroadcastConsumer(TVector<IDqOutput::TPtr>&& outputs, TMaybe<ui32> outputWidth)
        : Outputs(std::move(outputs))
        , OutputWidth(outputWidth)
        , Tmp(outputWidth.Defined() ? *outputWidth : 0u)
    {
    }

    bool IsFull() const override {
        return AnyOf(Outputs, [](const auto& output) { return output->IsFull(); });
    }

    void Consume(TUnboxedValue&& value) final {
        YQL_ENSURE(!OutputWidth.Defined());
        for (auto& output : Outputs) {
            TUnboxedValue copy{ value };
            output->Push(std::move(copy));
        }
    }

    void WideConsume(TUnboxedValue* values, ui32 count) final {
        YQL_ENSURE(OutputWidth.Defined() && OutputWidth == count);
        for (auto& output : Outputs) {
            std::copy(values, values + count, Tmp.begin());
            output->WidePush(Tmp.data(), count);
        }
    }

    void Consume(NDqProto::TCheckpoint&& checkpoint) override {
        for (auto& output : Outputs) {
            output->Push(NDqProto::TCheckpoint(checkpoint));
        }
    }

    void Finish() override {
        for (auto& output : Outputs) {
            output->Finish();
        }
    }

private:
    TVector<IDqOutput::TPtr> Outputs;
    const TMaybe<ui32> OutputWidth;
    TUnboxedValueVector Tmp;
};

} // namespace

IDqOutputConsumer::TPtr CreateOutputMultiConsumer(TVector<IDqOutputConsumer::TPtr>&& consumers) {
    return MakeIntrusive<TDqOutputMultiConsumer>(std::move(consumers));
}

IDqOutputConsumer::TPtr CreateOutputMapConsumer(IDqOutput::TPtr output) {
    return MakeIntrusive<TDqOutputMapConsumer>(output);
}

IDqOutputConsumer::TPtr CreateOutputHashPartitionConsumer(
    TVector<IDqOutput::TPtr>&& outputs,
    ui32 src, ui32 dst,
    TVector<TColumnInfo>&& keyColumns, const  NKikimr::NMiniKQL::TType* outputType,
    const NKikimr::NMiniKQL::THolderFactory& holderFactory,
    TMaybe<ui8> minFillPercentage,
    NUdf::IPgBuilder* pgBuilder)
{
    YQL_ENSURE(!outputs.empty());
    YQL_ENSURE(!keyColumns.empty());
    TMaybe<ui32> outputWidth;

    YQL_LOG(INFO) << "Create hash consumer func (src, dst) " << src << " " << dst; 
    #ifdef coordinator_v0
        return MakeIntrusive<TDqHistPartitionConsumer>(src, std::move(outputs), std::move(keyColumns), outputType);
    #endif
    
    if (outputType->IsMulti()) {
        outputWidth = static_cast<const NMiniKQL::TMultiType*>(outputType)->GetElementsCount();
    }

    if (AnyOf(keyColumns, [](const auto& info) { return !info.IsBlockOrScalar(); })) {
        return MakeIntrusive<TDqOutputHashPartitionConsumer>(std::move(outputs), std::move(keyColumns), outputWidth);
    }

    YQL_ENSURE(outputWidth.Defined(), "Expecting wide stream for block data");
    if (AllOf(keyColumns, [](const auto& info) { return *info.IsScalar; })) {
        // all key columns are scalars - all data will go to single output
        return MakeIntrusive<TDqOutputHashPartitionConsumerScalar>(std::move(outputs), std::move(keyColumns), outputType);
    }

    return MakeIntrusive<TDqOutputHashPartitionConsumerBlock>(std::move(outputs), std::move(keyColumns), outputType, holderFactory, minFillPercentage, pgBuilder);
}

IDqOutputConsumer::TPtr CreateOutputBroadcastConsumer(TVector<IDqOutput::TPtr>&& outputs, TMaybe<ui32> outputWidth) {
    return MakeIntrusive<TDqOutputBroadcastConsumer>(std::move(outputs), outputWidth);
}

} // namespace NYql::NDq

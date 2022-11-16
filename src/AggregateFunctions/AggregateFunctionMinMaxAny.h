#pragma once

#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>

#include <Columns/ColumnVector.h>
#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnNullable.h>
#include <DataTypes/IDataType.h>
#include <DataTypes/DataTypesNumber.h>
#include <base/StringRef.h>
#include <Common/assert_cast.h>
#include <DataTypes/DataTypeNullable.h>
#include <AggregateFunctions/IAggregateFunction.h>

#include "config.h"

#if USE_EMBEDDED_COMPILER
#    include <llvm/IR/IRBuilder.h>
#    include <DataTypes/Native.h>
#endif

namespace DB
{
struct Settings;

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NOT_IMPLEMENTED;
}

/** Aggregate functions that store one of passed values.
  * For example: min, max, any, anyLast.
  */


/// For numeric values.
template <typename T>
struct SingleValueDataFixed
{
private:
    using Self = SingleValueDataFixed;
    using ColVecType = ColumnVectorOrDecimal<T>;

    bool has_value = false; /// We need to remember if at least one value has been passed. This is necessary for AggregateFunctionIf.
    T value;

public:
    static constexpr bool is_nullable = false;
    static constexpr bool is_any = false;

    bool has() const
    {
        return has_value;
    }

    void insertResultInto(IColumn & to) const
    {
        if (has())
            assert_cast<ColVecType &>(to).getData().push_back(value);
        else
            assert_cast<ColVecType &>(to).insertDefault();
    }

    void write(WriteBuffer & buf, const ISerialization & /*serialization*/) const
    {
        writeBinary(has(), buf);
        if (has())
            writeBinary(value, buf);
    }

    void read(ReadBuffer & buf, const ISerialization & /*serialization*/, Arena *)
    {
        readBinary(has_value, buf);
        if (has())
            readBinary(value, buf);
    }


    void change(const IColumn & column, size_t row_num, Arena *)
    {
        has_value = true;
        value = assert_cast<const ColVecType &>(column).getData()[row_num];
    }

    /// Assuming to.has()
    void change(const Self & to, Arena *)
    {
        has_value = true;
        value = to.value;
    }

    bool changeFirstTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeFirstTime(const Self & to, Arena * arena)
    {
        if (!has() && to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeEveryTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        change(column, row_num, arena);
        return true;
    }

    bool changeEveryTime(const Self & to, Arena * arena)
    {
        if (to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has() || assert_cast<const ColVecType &>(column).getData()[row_num] < value)
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.value < value))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has() || assert_cast<const ColVecType &>(column).getData()[row_num] > value)
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.value > value))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool isEqualTo(const Self & to) const
    {
        return has() && to.value == value;
    }

    bool isEqualTo(const IColumn & column, size_t row_num) const
    {
        return has() && assert_cast<const ColVecType &>(column).getData()[row_num] == value;
    }

    static bool allocatesMemoryInArena()
    {
        return false;
    }

#if USE_EMBEDDED_COMPILER

    static constexpr bool is_compilable = true;

    static llvm::Value * getValuePtrFromAggregateDataPtr(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr)
    {
        llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

        static constexpr size_t value_offset_from_structure = offsetof(SingleValueDataFixed<T>, value);
        auto * value_ptr = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), aggregate_data_ptr, value_offset_from_structure);

        return value_ptr;
    }

    static llvm::Value * getValueFromAggregateDataPtr(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr)
    {
        llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

        auto * type = toNativeType<T>(builder);
        auto * value_ptr = getValuePtrFromAggregateDataPtr(builder, aggregate_data_ptr);

        return b.CreateLoad(type, value_ptr);
    }

    static void compileChange(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

        auto * has_value_ptr = aggregate_data_ptr;
        b.CreateStore(b.getInt1(true), has_value_ptr);

        auto * value_ptr = getValuePtrFromAggregateDataPtr(b, aggregate_data_ptr);
        b.CreateStore(value_to_check, value_ptr);
    }

    static void compileChangeMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        auto * value_src = getValueFromAggregateDataPtr(builder, aggregate_data_src_ptr);

        compileChange(builder, aggregate_data_dst_ptr, value_src);
    }

    static void compileChangeFirstTime(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

        auto * has_value_ptr = aggregate_data_ptr;
        auto * has_value_value = b.CreateLoad(b.getInt1Ty(), has_value_ptr);

        auto * head = b.GetInsertBlock();

        auto * join_block = llvm::BasicBlock::Create(head->getContext(), "join_block", head->getParent());
        auto * if_should_change = llvm::BasicBlock::Create(head->getContext(), "if_should_change", head->getParent());
        auto * if_should_not_change = llvm::BasicBlock::Create(head->getContext(), "if_should_not_change", head->getParent());

        b.CreateCondBr(has_value_value, if_should_not_change, if_should_change);

        b.SetInsertPoint(if_should_not_change);
        b.CreateBr(join_block);

        b.SetInsertPoint(if_should_change);
        compileChange(builder, aggregate_data_ptr, value_to_check);
        b.CreateBr(join_block);

        b.SetInsertPoint(join_block);
    }

    static void compileChangeFirstTimeMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

        auto * has_value_dst_ptr = aggregate_data_dst_ptr;
        auto * has_value_dst = b.CreateLoad(b.getInt1Ty(), has_value_dst_ptr);

        auto * has_value_src_ptr = aggregate_data_src_ptr;
        auto * has_value_src = b.CreateLoad(b.getInt1Ty(), has_value_src_ptr);

        auto * head = b.GetInsertBlock();

        auto * join_block = llvm::BasicBlock::Create(head->getContext(), "join_block", head->getParent());
        auto * if_should_change = llvm::BasicBlock::Create(head->getContext(), "if_should_change", head->getParent());
        auto * if_should_not_change = llvm::BasicBlock::Create(head->getContext(), "if_should_not_change", head->getParent());

        b.CreateCondBr(b.CreateAnd(b.CreateNot(has_value_dst), has_value_src), if_should_change, if_should_not_change);

        b.SetInsertPoint(if_should_change);
        compileChangeMerge(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
        b.CreateBr(join_block);

        b.SetInsertPoint(if_should_not_change);
        b.CreateBr(join_block);

        b.SetInsertPoint(join_block);
    }

    static void compileChangeEveryTime(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        compileChange(builder, aggregate_data_ptr, value_to_check);
    }

    static void compileChangeEveryTimeMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

        auto * has_value_src_ptr = aggregate_data_src_ptr;
        auto * has_value_src = b.CreateLoad(b.getInt1Ty(), has_value_src_ptr);

        auto * head = b.GetInsertBlock();

        auto * join_block = llvm::BasicBlock::Create(head->getContext(), "join_block", head->getParent());
        auto * if_should_change = llvm::BasicBlock::Create(head->getContext(), "if_should_change", head->getParent());
        auto * if_should_not_change = llvm::BasicBlock::Create(head->getContext(), "if_should_not_change", head->getParent());

        b.CreateCondBr(has_value_src, if_should_change, if_should_not_change);

        b.SetInsertPoint(if_should_change);
        compileChangeMerge(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
        b.CreateBr(join_block);

        b.SetInsertPoint(if_should_not_change);
        b.CreateBr(join_block);

        b.SetInsertPoint(join_block);
    }

    template <bool is_less>
    static void compileChangeComparison(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

        auto * has_value_ptr = aggregate_data_ptr;
        auto * has_value_value = b.CreateLoad(b.getInt1Ty(), has_value_ptr);

        auto * value = getValueFromAggregateDataPtr(b, aggregate_data_ptr);

        auto * head = b.GetInsertBlock();

        auto * join_block = llvm::BasicBlock::Create(head->getContext(), "join_block", head->getParent());
        auto * if_should_change = llvm::BasicBlock::Create(head->getContext(), "if_should_change", head->getParent());
        auto * if_should_not_change = llvm::BasicBlock::Create(head->getContext(), "if_should_not_change", head->getParent());

        auto is_signed = std::numeric_limits<T>::is_signed;

        llvm::Value * should_change_after_comparison = nullptr;

        if constexpr (is_less)
        {
            if (value_to_check->getType()->isIntegerTy())
                should_change_after_comparison = is_signed ? b.CreateICmpSLT(value_to_check, value) : b.CreateICmpULT(value_to_check, value);
            else
                should_change_after_comparison = b.CreateFCmpOLT(value_to_check, value);
        }
        else
        {
            if (value_to_check->getType()->isIntegerTy())
                should_change_after_comparison = is_signed ? b.CreateICmpSGT(value_to_check, value) : b.CreateICmpUGT(value_to_check, value);
            else
                should_change_after_comparison = b.CreateFCmpOGT(value_to_check, value);
        }

        b.CreateCondBr(b.CreateOr(b.CreateNot(has_value_value), should_change_after_comparison), if_should_change, if_should_not_change);

        b.SetInsertPoint(if_should_change);
        compileChange(builder, aggregate_data_ptr, value_to_check);
        b.CreateBr(join_block);

        b.SetInsertPoint(if_should_not_change);
        b.CreateBr(join_block);

        b.SetInsertPoint(join_block);
    }

    template <bool is_less>
    static void compileChangeComparisonMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

        auto * has_value_dst_ptr = aggregate_data_dst_ptr;
        auto * has_value_dst = b.CreateLoad(b.getInt1Ty(), has_value_dst_ptr);

        auto * value_dst = getValueFromAggregateDataPtr(b, aggregate_data_dst_ptr);

        auto * has_value_src_ptr = aggregate_data_src_ptr;
        auto * has_value_src = b.CreateLoad(b.getInt1Ty(), has_value_src_ptr);

        auto * value_src = getValueFromAggregateDataPtr(b, aggregate_data_src_ptr);

        auto * head = b.GetInsertBlock();

        auto * join_block = llvm::BasicBlock::Create(head->getContext(), "join_block", head->getParent());
        auto * if_should_change = llvm::BasicBlock::Create(head->getContext(), "if_should_change", head->getParent());
        auto * if_should_not_change = llvm::BasicBlock::Create(head->getContext(), "if_should_not_change", head->getParent());

        auto is_signed = std::numeric_limits<T>::is_signed;

        llvm::Value * should_change_after_comparison = nullptr;

        if constexpr (is_less)
        {
            if (value_src->getType()->isIntegerTy())
                should_change_after_comparison = is_signed ? b.CreateICmpSLT(value_src, value_dst) : b.CreateICmpULT(value_src, value_dst);
            else
                should_change_after_comparison = b.CreateFCmpOLT(value_src, value_dst);
        }
        else
        {
            if (value_src->getType()->isIntegerTy())
                should_change_after_comparison = is_signed ? b.CreateICmpSGT(value_src, value_dst) : b.CreateICmpUGT(value_src, value_dst);
            else
                should_change_after_comparison = b.CreateFCmpOGT(value_src, value_dst);
        }

        b.CreateCondBr(b.CreateAnd(has_value_src, b.CreateOr(b.CreateNot(has_value_dst), should_change_after_comparison)), if_should_change, if_should_not_change);

        b.SetInsertPoint(if_should_change);
        compileChangeMerge(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
        b.CreateBr(join_block);

        b.SetInsertPoint(if_should_not_change);
        b.CreateBr(join_block);

        b.SetInsertPoint(join_block);
    }

    static void compileChangeIfLess(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        static constexpr bool is_less = true;
        compileChangeComparison<is_less>(builder, aggregate_data_ptr, value_to_check);
    }

    static void compileChangeIfLessMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        static constexpr bool is_less = true;
        compileChangeComparisonMerge<is_less>(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
    }

    static void compileChangeIfGreater(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        static constexpr bool is_less = false;
        compileChangeComparison<is_less>(builder, aggregate_data_ptr, value_to_check);
    }

    static void compileChangeIfGreaterMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        static constexpr bool is_less = false;
        compileChangeComparisonMerge<is_less>(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
    }

    static llvm::Value * compileGetResult(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr)
    {
        return getValueFromAggregateDataPtr(builder, aggregate_data_ptr);
    }

#endif

};


/** For strings. Short strings are stored in the object itself, and long strings are allocated separately.
  * NOTE It could also be suitable for arrays of numbers.
  */
struct SingleValueDataString //-V730
{
private:
    using Self = SingleValueDataString;

    Int32 size = -1;    /// -1 indicates that there is no value.
    Int32 capacity = 0;    /// power of two or zero
    char * large_data;

public:
    static constexpr Int32 AUTOMATIC_STORAGE_SIZE = 64;
    static constexpr Int32 MAX_SMALL_STRING_SIZE = AUTOMATIC_STORAGE_SIZE - sizeof(size) - sizeof(capacity) - sizeof(large_data);

private:
    char small_data[MAX_SMALL_STRING_SIZE]; /// Including the terminating zero.

public:
    static constexpr bool is_nullable = false;
    static constexpr bool is_any = false;

    bool has() const { return size >= 1; }

    const char * getData() const
    {
        return size <= MAX_SMALL_STRING_SIZE ? small_data : large_data;
    }

    StringRef getStringRef() const
    {
        return StringRef(getData(), size);
    }

    void insertResultInto(IColumn & to) const
    {
        /// Same comment from write() applies here, but in this case we remove the last NULL character as we expect it to come from
        /// old statuses that always included it
        if (has())
        {
            const char * data = getData();
            size_t final_size = data[size - 1] != '\0' ? size : size - 1;
            if (final_size)
            {
                assert_cast<ColumnString &>(to).insertData(data, final_size);
                return;
            }
        }

        assert_cast<ColumnString &>(to).insertDefault();
    }

    void write(WriteBuffer & buf, const ISerialization & /*serialization*/) const
    {
        /// This used to be serialized using insertDataWithTerminatingZero up until 22.9 when it was changed to use insertData
        /// The problem is that this change, inadvertently, changed the serialization of the state, as insertDataWithTerminatingZero
        /// included the final NULL in its size count, while insertData doesn't. That is a string like "1234" would have been inserted
        /// as { size: 5, data: "1234\0" }, but with the change it was inserted as { size: 4, data: "1234\0" }-
        /// This changed in the stored size means that values created prior to this change, when read in an updated server, would
        /// consider the additional \0 as being part of the string; and that if you went backwards (downgrade or replication to an older
        /// server), newer values would remove the ending character of the string.
        ///
        /// To revert this change, the serialization has been changed yet again to replicate the old behaviour with one important
        /// caveat: As we can read both statuses with or without the change, we only add the extra +1 if we need it, as we decide if we
        /// need it based on whether the string is ended with a NULL (always on old statuses) or not (most of the time in newer statuses).
        /// The main problem with this approach is that there is a chance of newer status containing a NULL ending character, as it might
        /// have been part of the inserted string "naturally" (the data was inserted with a NULL)
        ///
        /// This should be taken as a patch applied to be as compatible as possible with older releases (with the NULL caveat), that is
        /// to be able to work with data generated by any prior release, with or without the intended change.
        /// In the future a new serialization **version** should be introduced replacing the current one in a backwards compatible manner.
        /// Refs: https://github.com/ClickHouse/ClickHouse/pull/43038 and https://github.com/ClickHouse/ClickHouse/issues/42916
        if (!has())
        {
            writeBinary(Int32{1}, buf);
            buf.write('\0');
            return;
        }

        const char * data = getData();
        if (data[size - 1] != '\0')
        {
            writeBinary(size + 1, buf);
            buf.write(data, size);
            buf.write('\0');
        }
        else
        {
            writeBinary(size, buf);
            buf.write(data, size);
        }
    }

    void read(ReadBuffer & buf, const ISerialization & /*serialization*/, Arena * arena)
    {
        Int32 rhs_size;
        readBinary(rhs_size, buf);

        if (rhs_size >= 0)
        {
            if (rhs_size <= MAX_SMALL_STRING_SIZE)
            {
                /// Don't free large_data here.

                size = rhs_size;

                if (size > 0)
                    buf.readStrict(small_data, size);
            }
            else
            {
                if (capacity < rhs_size)
                {
                    capacity = static_cast<UInt32>(roundUpToPowerOfTwoOrZero(rhs_size));
                    /// Don't free large_data here.
                    large_data = arena->alloc(capacity);
                }

                size = rhs_size;
                buf.readStrict(large_data, size);
            }
        }
        else
        {
            /// Don't free large_data here.
            size = rhs_size;
        }
    }

    /// Assuming to.has()
    void changeImpl(StringRef value, Arena * arena)
    {
        Int32 value_size = static_cast<Int32>(value.size);

        if (value_size <= MAX_SMALL_STRING_SIZE)
        {
            /// Don't free large_data here.
            size = value_size;

            if (size > 0)
                memcpy(small_data, value.data, size);
        }
        else
        {
            if (capacity < value_size)
            {
                /// Don't free large_data here.
                capacity = static_cast<Int32>(roundUpToPowerOfTwoOrZero(value_size));
                large_data = arena->alloc(capacity);
            }

            size = value_size;
            memcpy(large_data, value.data, size);
        }
    }

    void change(const IColumn & column, size_t row_num, Arena * arena)
    {
        changeImpl(assert_cast<const ColumnString &>(column).getDataAt(row_num), arena);
    }

    void change(const Self & to, Arena * arena)
    {
        changeImpl(to.getStringRef(), arena);
    }

    bool changeFirstTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeFirstTime(const Self & to, Arena * arena)
    {
        if (!has() && to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeEveryTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        change(column, row_num, arena);
        return true;
    }

    bool changeEveryTime(const Self & to, Arena * arena)
    {
        if (to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has() || assert_cast<const ColumnString &>(column).getDataAt(row_num) < getStringRef())
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.getStringRef() < getStringRef()))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has() || assert_cast<const ColumnString &>(column).getDataAt(row_num) > getStringRef())
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.getStringRef() > getStringRef()))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool isEqualTo(const Self & to) const
    {
        return has() && to.getStringRef() == getStringRef();
    }

    bool isEqualTo(const IColumn & column, size_t row_num) const
    {
        return has() && assert_cast<const ColumnString &>(column).getDataAt(row_num) == getStringRef();
    }

    static bool allocatesMemoryInArena()
    {
        return true;
    }

#if USE_EMBEDDED_COMPILER

    static constexpr bool is_compilable = false;

#endif

};

static_assert(
    sizeof(SingleValueDataString) == SingleValueDataString::AUTOMATIC_STORAGE_SIZE,
    "Incorrect size of SingleValueDataString struct");


/// For any other value types.
struct SingleValueDataGeneric
{
private:
    using Self = SingleValueDataGeneric;

    Field value;

public:
    static constexpr bool is_nullable = false;
    static constexpr bool is_any = false;

    bool has() const
    {
        return !value.isNull();
    }

    void insertResultInto(IColumn & to) const
    {
        if (has())
            to.insert(value);
        else
            to.insertDefault();
    }

    void write(WriteBuffer & buf, const ISerialization & serialization) const
    {
        if (!value.isNull())
        {
            writeBinary(true, buf);
            serialization.serializeBinary(value, buf);
        }
        else
            writeBinary(false, buf);
    }

    void read(ReadBuffer & buf, const ISerialization & serialization, Arena *)
    {
        bool is_not_null;
        readBinary(is_not_null, buf);

        if (is_not_null)
            serialization.deserializeBinary(value, buf);
    }

    void change(const IColumn & column, size_t row_num, Arena *)
    {
        column.get(row_num, value);
    }

    void change(const Self & to, Arena *)
    {
        value = to.value;
    }

    bool changeFirstTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
            return false;
    }

    bool changeFirstTime(const Self & to, Arena * arena)
    {
        if (!has() && to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeEveryTime(const IColumn & column, size_t row_num, Arena * arena)
    {
        change(column, row_num, arena);
        return true;
    }

    bool changeEveryTime(const Self & to, Arena * arena)
    {
        if (to.has())
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfLess(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
        {
            Field new_value;
            column.get(row_num, new_value);
            if (new_value < value)
            {
                value = new_value;
                return true;
            }
            else
                return false;
        }
    }

    bool changeIfLess(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.value < value))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool changeIfGreater(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (!has())
        {
            change(column, row_num, arena);
            return true;
        }
        else
        {
            Field new_value;
            column.get(row_num, new_value);
            if (new_value > value)
            {
                value = new_value;
                return true;
            }
            else
                return false;
        }
    }

    bool changeIfGreater(const Self & to, Arena * arena)
    {
        if (to.has() && (!has() || to.value > value))
        {
            change(to, arena);
            return true;
        }
        else
            return false;
    }

    bool isEqualTo(const IColumn & column, size_t row_num) const
    {
        return has() && value == column[row_num];
    }

    bool isEqualTo(const Self & to) const
    {
        return has() && to.value == value;
    }

    static bool allocatesMemoryInArena()
    {
        return false;
    }

#if USE_EMBEDDED_COMPILER

    static constexpr bool is_compilable = false;

#endif

};


/** What is the difference between the aggregate functions min, max, any, anyLast
  *  (the condition that the stored value is replaced by a new one,
  *   as well as, of course, the name).
  */

template <typename Data>
struct AggregateFunctionMinData : Data
{
    using Self = AggregateFunctionMinData;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)     { return this->changeIfLess(column, row_num, arena); }
    bool changeIfBetter(const Self & to, Arena * arena)                            { return this->changeIfLess(to, arena); }
    void addManyDefaults(const IColumn & column, size_t /*length*/, Arena * arena) { this->changeIfLess(column, 0, arena); }

    static const char * name() { return "min"; }

#if USE_EMBEDDED_COMPILER

    static constexpr bool is_compilable = Data::is_compilable;

    static void compileChangeIfBetter(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        Data::compileChangeIfLess(builder, aggregate_data_ptr, value_to_check);
    }

    static void compileChangeIfBetterMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        Data::compileChangeIfLessMerge(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
    }

#endif
};

template <typename Data>
struct AggregateFunctionMaxData : Data
{
    using Self = AggregateFunctionMaxData;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)     { return this->changeIfGreater(column, row_num, arena); }
    bool changeIfBetter(const Self & to, Arena * arena)                            { return this->changeIfGreater(to, arena); }
    void addManyDefaults(const IColumn & column, size_t /*length*/, Arena * arena) { this->changeIfGreater(column, 0, arena); }

    static const char * name() { return "max"; }

#if USE_EMBEDDED_COMPILER

    static constexpr bool is_compilable = Data::is_compilable;

    static void compileChangeIfBetter(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        Data::compileChangeIfGreater(builder, aggregate_data_ptr, value_to_check);
    }

    static void compileChangeIfBetterMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        Data::compileChangeIfGreaterMerge(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
    }

#endif
};

template <typename Data>
struct AggregateFunctionAnyData : Data
{
    using Self = AggregateFunctionAnyData;
    static constexpr bool is_any = true;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)     { return this->changeFirstTime(column, row_num, arena); }
    bool changeIfBetter(const Self & to, Arena * arena)                            { return this->changeFirstTime(to, arena); }
    void addManyDefaults(const IColumn & column, size_t /*length*/, Arena * arena) { this->changeFirstTime(column, 0, arena); }

    static const char * name() { return "any"; }

#if USE_EMBEDDED_COMPILER

    static constexpr bool is_compilable = Data::is_compilable;

    static void compileChangeIfBetter(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        Data::compileChangeFirstTime(builder, aggregate_data_ptr, value_to_check);
    }

    static void compileChangeIfBetterMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        Data::compileChangeFirstTimeMerge(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
    }

#endif
};

template <typename Data>
struct AggregateFunctionAnyLastData : Data
{
    using Self = AggregateFunctionAnyLastData;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)     { return this->changeEveryTime(column, row_num, arena); }
    bool changeIfBetter(const Self & to, Arena * arena)                            { return this->changeEveryTime(to, arena); }
    void addManyDefaults(const IColumn & column, size_t /*length*/, Arena * arena) { this->changeEveryTime(column, 0, arena); }

    static const char * name() { return "anyLast"; }

#if USE_EMBEDDED_COMPILER

    static constexpr bool is_compilable = Data::is_compilable;

    static void compileChangeIfBetter(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, llvm::Value * value_to_check)
    {
        Data::compileChangeEveryTime(builder, aggregate_data_ptr, value_to_check);
    }

    static void compileChangeIfBetterMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr)
    {
        Data::compileChangeEveryTimeMerge(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
    }

#endif
};

template <typename Data>
struct AggregateFunctionSingleValueOrNullData : Data
{
    static constexpr bool is_nullable = true;

    using Self = AggregateFunctionSingleValueOrNullData;

    bool first_value = true;
    bool is_null = false;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (first_value)
        {
            first_value = false;
            this->change(column, row_num, arena);
            return true;
        }
        else if (!this->isEqualTo(column, row_num))
        {
            is_null = true;
        }
        return false;
    }

    bool changeIfBetter(const Self & to, Arena * arena)
    {
        if (first_value)
        {
            first_value = false;
            this->change(to, arena);
            return true;
        }
        else if (!this->isEqualTo(to))
        {
            is_null = true;
        }
        return false;
    }

    void addManyDefaults(const IColumn & column, size_t /*length*/, Arena * arena) { this->changeIfBetter(column, 0, arena); }

    void insertResultInto(IColumn & to) const
    {
        if (is_null || first_value)
        {
            to.insertDefault();
        }
        else
        {
            ColumnNullable & col = typeid_cast<ColumnNullable &>(to);
            col.getNullMapColumn().insertDefault();
            this->Data::insertResultInto(col.getNestedColumn());
        }
    }

    static const char * name() { return "singleValueOrNull"; }

#if USE_EMBEDDED_COMPILER

    static constexpr bool is_compilable = false;

#endif
};

/** Implement 'heavy hitters' algorithm.
  * Selects most frequent value if its frequency is more than 50% in each thread of execution.
  * Otherwise, selects some arbitrary value.
  * http://www.cs.umd.edu/~samir/498/karp.pdf
  */
template <typename Data>
struct AggregateFunctionAnyHeavyData : Data
{
    UInt64 counter = 0;

    using Self = AggregateFunctionAnyHeavyData;

    bool changeIfBetter(const IColumn & column, size_t row_num, Arena * arena)
    {
        if (this->isEqualTo(column, row_num))
        {
            ++counter;
        }
        else
        {
            if (counter == 0)
            {
                this->change(column, row_num, arena);
                ++counter;
                return true;
            }
            else
                --counter;
        }
        return false;
    }

    bool changeIfBetter(const Self & to, Arena * arena)
    {
        if (this->isEqualTo(to))
        {
            counter += to.counter;
        }
        else
        {
            if ((!this->has() && to.has()) || counter < to.counter)
            {
                this->change(to, arena);
                return true;
            }
            else
                counter -= to.counter;
        }
        return false;
    }

    void addManyDefaults(const IColumn & column, size_t length, Arena * arena)
    {
        for (size_t i = 0; i < length; ++i)
            changeIfBetter(column, 0, arena);
    }

    void write(WriteBuffer & buf, const ISerialization & serialization) const
    {
        Data::write(buf, serialization);
        writeBinary(counter, buf);
    }

    void read(ReadBuffer & buf, const ISerialization & serialization, Arena * arena)
    {
        Data::read(buf, serialization, arena);
        readBinary(counter, buf);
    }

    static const char * name() { return "anyHeavy"; }

#if USE_EMBEDDED_COMPILER

    static constexpr bool is_compilable = false;

#endif

};


template <typename Data>
class AggregateFunctionsSingleValue final : public IAggregateFunctionDataHelper<Data, AggregateFunctionsSingleValue<Data>>
{
    static constexpr bool is_any = Data::is_any;

private:
    SerializationPtr serialization;

public:
    explicit AggregateFunctionsSingleValue(const DataTypePtr & type)
        : IAggregateFunctionDataHelper<Data, AggregateFunctionsSingleValue<Data>>({type}, {})
        , serialization(type->getDefaultSerialization())
    {
        if (StringRef(Data::name()) == StringRef("min")
            || StringRef(Data::name()) == StringRef("max"))
        {
            if (!type->isComparable())
                throw Exception("Illegal type " + type->getName() + " of argument of aggregate function " + getName()
                    + " because the values of that data type are not comparable", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }
    }

    String getName() const override { return Data::name(); }

    DataTypePtr getReturnType() const override
    {
        auto result_type = this->argument_types.at(0);
        if constexpr (Data::is_nullable)
            return makeNullable(result_type);
        return result_type;
    }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena * arena) const override
    {
        this->data(place).changeIfBetter(*columns[0], row_num, arena);
    }

    void addManyDefaults(
        AggregateDataPtr __restrict place,
        const IColumn ** columns,
        size_t length,
        Arena * arena) const override
    {
        this->data(place).addManyDefaults(*columns[0], length, arena);
    }

    void addBatchSinglePlace(
        size_t row_begin,
        size_t row_end,
        AggregateDataPtr place,
        const IColumn ** columns,
        Arena * arena,
        ssize_t if_argument_pos) const override
    {
        if constexpr (is_any)
            if (this->data(place).has())
                return;
        if (if_argument_pos >= 0)
        {
            const auto & flags = assert_cast<const ColumnUInt8 &>(*columns[if_argument_pos]).getData();
            for (size_t i = row_begin; i < row_end; ++i)
            {
                if (flags[i])
                {
                    this->data(place).changeIfBetter(*columns[0], i, arena);
                    if constexpr (is_any)
                        break;
                }
            }
        }
        else
        {
            for (size_t i = row_begin; i < row_end; ++i)
            {
                this->data(place).changeIfBetter(*columns[0], i, arena);
                if constexpr (is_any)
                    break;
            }
        }
    }

    void addBatchSinglePlaceNotNull( /// NOLINT
        size_t row_begin,
        size_t row_end,
        AggregateDataPtr place,
        const IColumn ** columns,
        const UInt8 * null_map,
        Arena * arena,
        ssize_t if_argument_pos = -1) const override
    {
        if constexpr (is_any)
            if (this->data(place).has())
                return;

        if (if_argument_pos >= 0)
        {
            const auto & flags = assert_cast<const ColumnUInt8 &>(*columns[if_argument_pos]).getData();
            for (size_t i = row_begin; i < row_end; ++i)
            {
                if (!null_map[i] && flags[i])
                {
                    this->data(place).changeIfBetter(*columns[0], i, arena);
                    if constexpr (is_any)
                        break;
                }
            }
        }
        else
        {
            for (size_t i = row_begin; i < row_end; ++i)
            {
                if (!null_map[i])
                {
                    this->data(place).changeIfBetter(*columns[0], i, arena);
                    if constexpr (is_any)
                        break;
                }
            }
        }
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena * arena) const override
    {
        this->data(place).changeIfBetter(this->data(rhs), arena);
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        this->data(place).write(buf, *serialization);
    }

    void deserialize(AggregateDataPtr place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena * arena) const override
    {
        this->data(place).read(buf, *serialization, arena);
    }

    bool allocatesMemoryInArena() const override
    {
        return Data::allocatesMemoryInArena();
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        this->data(place).insertResultInto(to);
    }

#if USE_EMBEDDED_COMPILER

    bool isCompilable() const override
    {
        if constexpr (!Data::is_compilable)
            return false;

        return canBeNativeType(*this->argument_types[0]);
    }


    void compileCreate(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr) const override
    {
        llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

        b.CreateMemSet(aggregate_data_ptr, llvm::ConstantInt::get(b.getInt8Ty(), 0), this->sizeOfData(), llvm::assumeAligned(this->alignOfData()));
    }

    void compileAdd(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr, const DataTypes &, const std::vector<llvm::Value *> & argument_values) const override
    {
        if constexpr (Data::is_compilable)
        {
            Data::compileChangeIfBetter(builder, aggregate_data_ptr, argument_values[0]);
        }
        else
        {
            throw Exception(getName() + " is not JIT-compilable", ErrorCodes::NOT_IMPLEMENTED);
        }
    }

    void compileMerge(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_dst_ptr, llvm::Value * aggregate_data_src_ptr) const override
    {
        if constexpr (Data::is_compilable)
        {
            Data::compileChangeIfBetterMerge(builder, aggregate_data_dst_ptr, aggregate_data_src_ptr);
        }
        else
        {
            throw Exception(getName() + " is not JIT-compilable", ErrorCodes::NOT_IMPLEMENTED);
        }
    }

    llvm::Value * compileGetResult(llvm::IRBuilderBase & builder, llvm::Value * aggregate_data_ptr) const override
    {
        if constexpr (Data::is_compilable)
        {
            return Data::compileGetResult(builder, aggregate_data_ptr);
        }
        else
        {
            throw Exception(getName() + " is not JIT-compilable", ErrorCodes::NOT_IMPLEMENTED);
        }
    }

#endif
};

}

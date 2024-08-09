#pragma once

#include <Columns/IColumn.h>
#include <Core/Block.h>
#include <Common/PODArray.h>
#include "base/defines.h"

#include <boost/math/distributions/fwd.hpp>
#include <boost/noncopyable.hpp>

#include <Poco/Logger.h>
#include <Common/logger_useful.h>

namespace DB
{

namespace detail
{

class Selector
{
public:
    using Range = std::pair<size_t, size_t>;

    /// [begin, end)
    Selector(size_t begin, size_t end) : data(Range{begin, end}) { }
    Selector() : Selector(0, 0) { }

    Selector(IColumn::Selector && selector_) : data(initializeFromSelector(std::move(selector_))) { }

    class Iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = size_t *;
        using reference = size_t &;

        Iterator(const Selector & selector_, size_t idx_) : selector(selector_), idx(idx_) { }

        size_t operator*() const
        {
            chassert(idx < selector.size());
            if (idx >= selector.size())
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Index {} out of range size {}", idx, selector.size());
            return selector[idx];
        }

        Iterator & operator++()
        {
            if (idx >= selector.size())
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Index {} out of range size {}", idx, selector.size());
            ++idx;
            return *this;
        }

        bool operator!=(const Iterator & other) const { return idx != other.idx; }

    private:
        const Selector & selector;
        size_t idx;
    };

    Iterator begin() const { return Iterator(*this, 0); }

    Iterator end() const { return Iterator(*this, size()); }

    size_t operator[](size_t idx) const
    {
        chassert(idx < size());
        if (idx >= size())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Index {} out of range size {}", idx, size());

        if (std::holds_alternative<Range>(data))
        {
            auto range = std::get<Range>(data);
            return range.first + idx;
        }
        else
        {
            return std::get<IColumn::Selector>(data)[idx];
        }
    }

    size_t size() const
    {
        if (std::holds_alternative<Range>(data))
        {
            auto range = std::get<Range>(data);
            return range.second - range.first;
        }
        else
        {
            return std::get<IColumn::Selector>(data).size();
        }
    }

    std::pair<Selector, Selector> split(size_t num_rows)
    {
        if (num_rows > size())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Index {} out of range size {}", num_rows, size());

        if (std::holds_alternative<Range>(data))
        {
            auto range = std::get<Range>(data);

            if (num_rows == 0)
                return {Selector(), Selector{range.first, range.second}};

            if (num_rows == size())
                return {Selector{range.first, range.second}, Selector()};

            return {Selector(range.first, range.first + num_rows), Selector(range.first + num_rows, range.second)};
        }
        else
        {
            auto & selector = std::get<IColumn::Selector>(data);
            return {
                Selector(IColumn::Selector(selector.begin(), selector.begin() + num_rows)),
                Selector(IColumn::Selector(selector.begin() + num_rows, selector.end()))};
        }
    }

private:
    using Data = std::variant<Range, IColumn::Selector>;

    Data initializeFromSelector(IColumn::Selector && selector)
    {
        if (selector.empty())
            return Range{0, 0};

        /// selector represents continuous range
        if (selector.back() == selector.front() + selector.size() - 1)
            return Range{selector.front(), selector.front() + selector.size()};

        return std::move(selector);
    }

    Data data;
};

}

struct ScatteredBlock : private boost::noncopyable
{
    using Selector = detail::Selector;

    ScatteredBlock() = default;

    explicit ScatteredBlock(Block block_) : block(std::move(block_)), selector(createTrivialSelector(block.rows())) { }

    ScatteredBlock(Block block_, IColumn::Selector && selector_) : block(std::move(block_)), selector(std::move(selector_)) { }

    ScatteredBlock(Block block_, Selector selector_) : block(std::move(block_)), selector(std::move(selector_)) { }

    ScatteredBlock(ScatteredBlock && other) noexcept : block(std::move(other.block)), selector(std::move(other.selector))
    {
        other.block.clear();
        other.selector = {};
    }

    ScatteredBlock & operator=(ScatteredBlock && other) noexcept
    {
        if (this != &other)
        {
            block = std::move(other.block);
            selector = std::move(other.selector);

            other.block.clear();
            other.selector = {};
        }
        return *this;
    }

    Block & getSourceBlock() & { return block; }
    const Block & getSourceBlock() const & { return block; }

    Block && getSourceBlock() && { return std::move(block); }

    const auto & getSelector() const { return selector; }

    bool contains(size_t idx) const { return std::find(selector.begin(), selector.end(), idx) != selector.end(); }

    explicit operator bool() const { return !!block; }

    /// Accounts only selected rows
    size_t rows() const { return selector.size(); }

    /// Whether block was scattered, i.e. has non-trivial selector
    bool wasScattered() const
    {
        chassert(block);
        return selector.size() != block.rows();
    }

    const ColumnWithTypeAndName & getByName(const std::string & name) const
    {
        chassert(block);
        return block.getByName(name);
    }

    /// Filters selector by mask discarding rows for which filter is false
    void filter(const IColumn::Filter & filter)
    {
        chassert(block && block.rows() == filter.size());
        IColumn::Selector new_selector;
        new_selector.reserve(selector.size());
        std::copy_if(selector.begin(), selector.end(), std::back_inserter(new_selector), [&](size_t idx) { return filter[idx]; });
        selector = std::move(new_selector);
    }

    /// Applies selector to block in place
    void filterBySelector()
    {
        chassert(block);

        if (!wasScattered())
            return;

        auto columns = block.getColumns();
        for (auto & col : columns)
        {
            auto c = col->cloneEmpty();
            c->reserve(selector.size());
            /// TODO: create new method in IColumnHelper to devirtualize
            for (const auto idx : selector)
                c->insertFrom(*col, idx);
            col = std::move(c);
        }

        /// We have to to id that way because references to the block should remain valid
        block.setColumns(columns);
        selector = createTrivialSelector(block.rows());
    }

    /// Cut first num_rows rows from block in place and returns block with remaining rows
    ScatteredBlock cut(size_t num_rows)
    {
        SCOPE_EXIT(filterBySelector());

        if (num_rows >= rows())
            return ScatteredBlock{block.cloneEmpty()};

        chassert(block);

        LOG_DEBUG(&Poco::Logger::get("debug"), "selector=({})", fmt::join(selector, ","));

        auto && [first_num_rows, remaining_selector] = selector.split(num_rows);

        LOG_DEBUG(
            &Poco::Logger::get("debug"),
            "first_num_rows=({}), remaining_selector=({})",
            fmt::join(first_num_rows, ","),
            fmt::join(remaining_selector, ","));

        auto remaining = ScatteredBlock{block, std::move(remaining_selector)};

        selector = std::move(first_num_rows);

        return remaining;
    }

    void replicate(const IColumn::Offsets & offsets, size_t existing_columns, const std::vector<size_t> & right_keys_to_replicate)
    {
        chassert(block);
        chassert(offsets.size() == rows());

        auto columns = block.getColumns();
        for (size_t i = 0; i < existing_columns; ++i)
        {
            auto c = columns[i]->replicate(offsets);
            columns[i] = std::move(c);
        }
        for (size_t pos : right_keys_to_replicate)
        {
            auto c = columns[pos]->replicate(offsets);
            columns[pos] = std::move(c);
        }

        block.setColumns(columns);
        selector = createTrivialSelector(block.rows());
    }

private:
    Selector createTrivialSelector(size_t size) { return Selector(0, size - 1); }

    Block block;
    Selector selector;
};

using ScatteredBlocks = std::vector<ScatteredBlock>;

}

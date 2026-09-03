#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>

namespace qtrader
{
	enum class FixedHashInsertResult : std::uint8_t
	{
		Inserted,
		Duplicate,
		Full,
		InvalidKey
	};

	// A bounded, startup-allocated open-addressing table for single-thread hot
	// paths. Key zero is reserved as the empty marker. No insert/erase allocates.
	template<typename Value>
	class FixedHashTable
	{
		static_assert(std::is_trivially_copyable<Value>::value,
			"fixed hash values must be trivially copyable");

		struct Entry
		{
			std::uint64_t key{0};
			Value value{};
		};

	public:
		explicit FixedHashTable(std::size_t maximum_entries)
			: maximum_entries_(maximum_entries),
			  capacity_(rounded_capacity(maximum_entries)),
			  entries_(static_cast<Entry*>(::operator new[](
				  sizeof(Entry) * capacity_, std::align_val_t(64))))
		{
			for (std::size_t index = 0; index < capacity_; ++index)
				new (entries_ + index) Entry{};
		}

		~FixedHashTable()
		{
			for (std::size_t index = 0; index < capacity_; ++index)
				entries_[index].~Entry();
			::operator delete[](entries_, std::align_val_t(64));
		}

		FixedHashTable(const FixedHashTable&) = delete;
		FixedHashTable& operator=(const FixedHashTable&) = delete;
		FixedHashTable(FixedHashTable&&) = delete;
		FixedHashTable& operator=(FixedHashTable&&) = delete;

		FixedHashInsertResult insert(std::uint64_t key,
			const Value& value) noexcept
		{
			if (key == 0)
				return FixedHashInsertResult::InvalidKey;
			if (size_ == maximum_entries_)
				return find(key) == nullptr ? FixedHashInsertResult::Full :
					FixedHashInsertResult::Duplicate;
			std::size_t slot = bucket(key);
			for (std::size_t probe = 0; probe < capacity_; ++probe)
			{
				if (entries_[slot].key == key)
					return FixedHashInsertResult::Duplicate;
				if (entries_[slot].key == 0)
				{
					entries_[slot].key = key;
					entries_[slot].value = value;
					++size_;
					return FixedHashInsertResult::Inserted;
				}
				slot = (slot + 1) & (capacity_ - 1);
			}
			return FixedHashInsertResult::Full;
		}

		Value* find(std::uint64_t key) noexcept
		{
			const auto slot = find_slot(key);
			return slot == capacity_ ? nullptr : &entries_[slot].value;
		}

		const Value* find(std::uint64_t key) const noexcept
		{
			const auto slot = find_slot(key);
			return slot == capacity_ ? nullptr : &entries_[slot].value;
		}

		bool erase(std::uint64_t key) noexcept
		{
			const auto slot = find_slot(key);
			if (slot == capacity_)
				return false;
			backshift_from(slot);
			--size_;
			return true;
		}

		std::size_t size() const noexcept { return size_; }
		std::size_t maximum_entries() const noexcept
		{
			return maximum_entries_;
		}
		std::size_t allocated_bytes() const noexcept
		{
			return capacity_ * sizeof(Entry);
		}

	private:
		static std::size_t rounded_capacity(std::size_t maximum_entries)
		{
			if (maximum_entries == 0 ||
				maximum_entries > std::numeric_limits<std::size_t>::max() / 2)
				throw std::bad_array_new_length{};
			std::size_t capacity = 8;
			while (capacity < maximum_entries * 2)
			{
				if (capacity > std::numeric_limits<std::size_t>::max() / 2)
					throw std::bad_array_new_length{};
				capacity <<= 1;
			}
			return capacity;
		}

		static std::uint64_t hash(std::uint64_t value) noexcept
		{
			value ^= value >> 33;
			value *= 0xff51afd7ed558ccdULL;
			value ^= value >> 33;
			return value;
		}

		std::size_t bucket(std::uint64_t key) const noexcept
		{
			return static_cast<std::size_t>(hash(key)) & (capacity_ - 1);
		}

		std::size_t find_slot(std::uint64_t key) const noexcept
		{
			if (key == 0)
				return capacity_;
			std::size_t slot = bucket(key);
			for (std::size_t probe = 0; probe < capacity_; ++probe)
			{
				if (entries_[slot].key == 0)
					return capacity_;
				if (entries_[slot].key == key)
					return slot;
				slot = (slot + 1) & (capacity_ - 1);
			}
			return capacity_;
		}

		void backshift_from(std::size_t hole) noexcept
		{
			const auto mask = capacity_ - 1;
			std::size_t scan = (hole + 1) & mask;
			while (entries_[scan].key != 0)
			{
				const auto home = bucket(entries_[scan].key);
				const auto scan_distance = (scan - home) & mask;
				const auto hole_distance = (hole - home) & mask;
				if (hole_distance < scan_distance)
				{
					entries_[hole] = entries_[scan];
					hole = scan;
				}
				scan = (scan + 1) & mask;
			}
			entries_[hole] = Entry{};
		}

		std::size_t maximum_entries_{0};
		std::size_t capacity_{0};
		Entry* entries_{nullptr};
		std::size_t size_{0};
	};

	struct FixedHashSetValue
	{
		std::uint8_t present{1};
	};

	using FixedHashSet = FixedHashTable<FixedHashSetValue>;
}

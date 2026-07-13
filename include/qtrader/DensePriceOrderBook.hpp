#pragma once

#include "PriceTimeOrderBook.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace wthft
{
	template<typename T>
	class AlignedArray
	{
	public:
		explicit AlignedArray(std::size_t size) : _size(size)
		{
			_data = static_cast<T*>(::operator new[](sizeof(T) * size, std::align_val_t(64)));
			for (std::size_t i = 0; i < size; ++i) new(_data + i) T();
		}

		~AlignedArray()
		{
			for (std::size_t i = 0; i < _size; ++i) _data[i].~T();
			::operator delete[](_data, std::align_val_t(64));
		}

		AlignedArray(const AlignedArray&) = delete;
		AlignedArray& operator=(const AlignedArray&) = delete;
		T& operator[](std::size_t index) { return _data[index]; }
		const T& operator[](std::size_t index) const { return _data[index]; }
		T* data() { return _data; }
		const T* data() const { return _data; }

	private:
		T* _data;
		std::size_t _size;
	};

	class FixedOrderIndex
	{
		static constexpr uint64_t kEmpty = 0;
		static constexpr uint64_t kDeleted = std::numeric_limits<uint64_t>::max();

	public:
		explicit FixedOrderIndex(std::size_t orders)
			: _capacity(rounded_capacity(orders)), _entries(_capacity)
		{
			for (std::size_t i = 0; i < _capacity; ++i)
				_entries[i] = Entry{};
		}

		bool insert(uint64_t key, uint32_t value)
		{
			std::size_t slot = hash(key) & (_capacity - 1);
			std::size_t deleted = _capacity;
			for (std::size_t probe = 0; probe < _capacity; ++probe)
			{
				if (_entries[slot].key == key) return false;
				if (_entries[slot].key == kDeleted && deleted == _capacity) deleted = slot;
				if (_entries[slot].key == kEmpty)
				{
					const std::size_t target = deleted == _capacity ? slot : deleted;
					_entries[target].key = key;
					_entries[target].value = value;
					return true;
				}
				slot = (slot + 1) & (_capacity - 1);
			}
			if (deleted != _capacity)
			{
				_entries[deleted].key = key;
				_entries[deleted].value = value;
				return true;
			}
			return false;
		}

		uint32_t find(uint64_t key) const
		{
			if (key == kEmpty || key == kDeleted) return invalid_index();
			std::size_t slot = hash(key) & (_capacity - 1);
			for (std::size_t probe = 0; probe < _capacity; ++probe)
			{
				if (_entries[slot].key == kEmpty) return invalid_index();
				if (_entries[slot].key == key) return _entries[slot].value;
				slot = (slot + 1) & (_capacity - 1);
			}
			return invalid_index();
		}

		void erase(uint64_t key)
		{
			std::size_t slot = hash(key) & (_capacity - 1);
			for (std::size_t probe = 0; probe < _capacity; ++probe)
			{
				if (_entries[slot].key == kEmpty) return;
				if (_entries[slot].key == key)
				{
					_entries[slot].key = kDeleted;
					return;
				}
				slot = (slot + 1) & (_capacity - 1);
			}
		}

		static constexpr uint32_t invalid_index() { return std::numeric_limits<uint32_t>::max(); }

	private:
		struct Entry
		{
			uint64_t key = kEmpty;
			uint32_t value = 0;
			uint32_t reserved = 0;
		};
		static_assert(sizeof(Entry) == 16, "order index entry must remain cache compact");

		static std::size_t rounded_capacity(std::size_t orders)
		{
			std::size_t result = 8;
			while (result < orders * 4) result <<= 1;
			return result;
		}

		static uint64_t hash(uint64_t value)
		{
			return value ^ (value >> 32);
		}

		std::size_t _capacity;
		AlignedArray<Entry> _entries;
	};

	class DensePriceOrderBook
	{
		static constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();

		struct Node
		{
			uint64_t id = 0;
			int64_t price = 0;
			uint64_t remaining = 0;
			uint32_t previous = kInvalid;
			uint32_t next = kInvalid;
			uint32_t free_next = kInvalid;
			uint32_t level = kInvalid;
			Side side = Side::Buy;
			OrderState state = OrderState::New;
		};

		struct PriceLevel
		{
			uint32_t head = kInvalid;
			uint32_t tail = kInvalid;
			uint64_t total_quantity = 0;
		};

	public:
		DensePriceOrderBook(std::size_t order_capacity, int64_t minimum_price,
			int64_t tick_size, uint32_t level_count)
			: _nodes(order_capacity), _bids(level_count), _asks(level_count),
			  _bid_bitmap((level_count + 63) / 64), _ask_bitmap((level_count + 63) / 64),
			  _index(order_capacity), _order_capacity(order_capacity),
			  _minimum_price(minimum_price), _tick_size(tick_size), _level_count(level_count),
			  _free(order_capacity == 0 ? kInvalid : 0), _active(0),
			  _best_bid(kInvalid), _best_ask(kInvalid)
		{
			for (uint32_t i = 0; i < order_capacity; ++i)
				_nodes[i].free_next = i + 1 < order_capacity ? i + 1 : kInvalid;
			for (std::size_t i = 0; i < (_level_count + 63) / 64; ++i)
				_bid_bitmap[i] = _ask_bitmap[i] = 0;
		}

		template<typename Sink>
		bool submit(const OrderRequest& request, Sink& sink)
		{
			uint32_t level = kInvalid;
			if (request.id == 0 || request.id == std::numeric_limits<uint64_t>::max() ||
				request.quantity == 0 || _index.find(request.id) != kInvalid ||
				(request.type == OrderType::Limit && !price_index(request.price, level)))
			{
				sink.on_order(OrderUpdate{request.id, OrderState::Rejected, request.quantity});
				return false;
			}

			const uint32_t index = allocate();
			if (index == kInvalid || !_index.insert(request.id, index))
			{
				if (index != kInvalid) release_without_index(index);
				sink.on_order(OrderUpdate{request.id, OrderState::Rejected, request.quantity});
				return false;
			}
			Node& order = _nodes[index];
			order.id = request.id;
			order.side = request.side;
			order.price = request.price;
			order.remaining = request.quantity;
			order.state = OrderState::New;
			order.level = level;

			match(index, request.type, sink);
			if (order.remaining == 0)
			{
				order.state = OrderState::Filled;
				sink.on_order(OrderUpdate{order.id, order.state, 0});
				release(index);
				return true;
			}
			if (request.type == OrderType::Market)
			{
				order.state = OrderState::Canceled;
				sink.on_order(OrderUpdate{order.id, order.state, order.remaining});
				release(index);
				return true;
			}

			order.state = order.remaining == request.quantity ? OrderState::New : OrderState::PartiallyFilled;
			append(index, level);
			sink.on_order(OrderUpdate{order.id, order.state, order.remaining});
			return true;
		}

		template<typename Sink>
		bool cancel(uint64_t id, Sink& sink)
		{
			const uint32_t index = _index.find(id);
			if (index == kInvalid) return false;
			Node& order = _nodes[index];
			unlink(index);
			order.state = OrderState::Canceled;
			sink.on_order(OrderUpdate{id, order.state, order.remaining});
			release(index);
			return true;
		}

		template<typename Sink>
		bool modify(uint64_t id, int64_t price, uint64_t quantity, Sink& sink)
		{
			const uint32_t index = _index.find(id);
			uint32_t level = kInvalid;
			if (index == kInvalid || quantity == 0 || !price_index(price, level)) return false;
			Node& order = _nodes[index];
			unlink(index);
			order.price = price;
			order.remaining = quantity;
			order.state = OrderState::New;
			order.level = level;
			match(index, OrderType::Limit, sink);
			if (order.remaining == 0)
			{
				order.state = OrderState::Filled;
				sink.on_order(OrderUpdate{order.id, order.state, 0});
				release(index);
				return true;
			}
			order.state = order.remaining == quantity ? OrderState::New : OrderState::PartiallyFilled;
			append(index, level);
			sink.on_order(OrderUpdate{order.id, order.state, order.remaining});
			return true;
		}

		std::size_t active_orders() const { return _active; }
		int64_t best_bid() const { return _best_bid == kInvalid ? 0 : level_price(_best_bid); }
		int64_t best_ask() const { return _best_ask == kInvalid ? 0 : level_price(_best_ask); }
		bool contains(uint64_t id) const { return _index.find(id) != kInvalid; }
		const void* node_storage() const { return _nodes.data(); }

	private:
		bool price_index(int64_t price, uint32_t& output) const
		{
			if (_tick_size <= 0 || price < _minimum_price) return false;
			const int64_t delta = price - _minimum_price;
			uint64_t index;
			if (_tick_size == 1)
				index = static_cast<uint64_t>(delta);
			else
			{
				if (delta % _tick_size != 0) return false;
				index = static_cast<uint64_t>(delta / _tick_size);
			}
			if (index >= _level_count) return false;
			output = static_cast<uint32_t>(index);
			return true;
		}

		int64_t level_price(uint32_t level) const
		{
			return _minimum_price + static_cast<int64_t>(level) * _tick_size;
		}

		uint32_t allocate()
		{
			if (_free == kInvalid) return kInvalid;
			const uint32_t index = _free;
			_free = _nodes[index].free_next;
			_nodes[index] = Node{};
			++_active;
			return index;
		}

		void release_without_index(uint32_t index)
		{
			_nodes[index].free_next = _free;
			_free = index;
			--_active;
		}

		void release(uint32_t index)
		{
			_index.erase(_nodes[index].id);
			release_without_index(index);
		}

		void append(uint32_t index, uint32_t level_index)
		{
			Node& order = _nodes[index];
			PriceLevel& level = order.side == Side::Buy ? _bids[level_index] : _asks[level_index];
			order.previous = level.tail;
			if (level.tail != kInvalid) _nodes[level.tail].next = index;
			else level.head = index;
			level.tail = index;
			level.total_quantity += order.remaining;
			set_occupied(order.side, level_index);
		}

		void unlink(uint32_t index)
		{
			Node& order = _nodes[index];
			const uint32_t level_index = order.level;
			PriceLevel& level = order.side == Side::Buy ? _bids[level_index] : _asks[level_index];
			if (order.previous != kInvalid) _nodes[order.previous].next = order.next;
			else level.head = order.next;
			if (order.next != kInvalid) _nodes[order.next].previous = order.previous;
			else level.tail = order.previous;
			level.total_quantity -= order.remaining;
			order.previous = order.next = kInvalid;
			if (level.head == kInvalid) clear_occupied(order.side, level_index);
		}

		void set_occupied(Side side, uint32_t level)
		{
			AlignedArray<uint64_t>& bitmap = side == Side::Buy ? _bid_bitmap : _ask_bitmap;
			bitmap[level >> 6] |= 1ULL << (level & 63);
			if (side == Side::Buy)
			{
				if (_best_bid == kInvalid || level > _best_bid) _best_bid = level;
			}
			else if (_best_ask == kInvalid || level < _best_ask) _best_ask = level;
		}

		void clear_occupied(Side side, uint32_t level)
		{
			AlignedArray<uint64_t>& bitmap = side == Side::Buy ? _bid_bitmap : _ask_bitmap;
			bitmap[level >> 6] &= ~(1ULL << (level & 63));
			if (side == Side::Buy && _best_bid == level) _best_bid = find_best(_bid_bitmap, true);
			if (side == Side::Sell && _best_ask == level) _best_ask = find_best(_ask_bitmap, false);
		}

		uint32_t find_best(const AlignedArray<uint64_t>& bitmap, bool highest) const
		{
			const uint32_t words = (_level_count + 63) / 64;
			if (highest)
			{
				for (uint32_t word = words; word-- > 0;)
				{
					const uint64_t bits = bitmap[word];
					if (bits != 0) return word * 64 + highest_bit(bits);
				}
			}
			else
			{
				for (uint32_t word = 0; word < words; ++word)
				{
					const uint64_t bits = bitmap[word];
					if (bits != 0) return word * 64 + lowest_bit(bits);
				}
			}
			return kInvalid;
		}

		static uint32_t lowest_bit(uint64_t bits)
		{
			#if defined(__GNUC__) || defined(__clang__)
			return static_cast<uint32_t>(__builtin_ctzll(bits));
			#else
			uint32_t index = 0;
			while ((bits & 1ULL) == 0) { bits >>= 1; ++index; }
			return index;
			#endif
		}

		static uint32_t highest_bit(uint64_t bits)
		{
			#if defined(__GNUC__) || defined(__clang__)
			return 63U - static_cast<uint32_t>(__builtin_clzll(bits));
			#else
			uint32_t index = 0;
			while (bits >>= 1) ++index;
			return index;
			#endif
		}

		template<typename Sink>
		void match(uint32_t taker_index, OrderType type, Sink& sink)
		{
			Node& taker = _nodes[taker_index];
			while (taker.remaining != 0)
			{
				const uint32_t level_index = taker.side == Side::Buy ? _best_ask : _best_bid;
				if (level_index == kInvalid) return;
				const int64_t price = level_price(level_index);
				if (type == OrderType::Limit &&
					(taker.side == Side::Buy ? taker.price < price : taker.price > price)) return;
				PriceLevel& level = taker.side == Side::Buy ? _asks[level_index] : _bids[level_index];
				while (taker.remaining != 0 && level.head != kInvalid)
				{
					const uint32_t maker_index = level.head;
					Node& maker = _nodes[maker_index];
					const uint64_t quantity = taker.remaining < maker.remaining ? taker.remaining : maker.remaining;
					taker.remaining -= quantity;
					maker.remaining -= quantity;
					level.total_quantity -= quantity;
					sink.on_trade(Trade{maker.id, taker.id, price, quantity});
					if (maker.remaining == 0)
					{
						maker.state = OrderState::Filled;
						sink.on_order(OrderUpdate{maker.id, maker.state, 0});
						unlink_filled(maker_index, level_index);
						release(maker_index);
					}
					else
					{
						maker.state = OrderState::PartiallyFilled;
						sink.on_order(OrderUpdate{maker.id, maker.state, maker.remaining});
					}
				}
			}
		}

		void unlink_filled(uint32_t index, uint32_t level_index)
		{
			Node& order = _nodes[index];
			PriceLevel& level = order.side == Side::Buy ? _bids[level_index] : _asks[level_index];
			level.head = order.next;
			if (order.next != kInvalid) _nodes[order.next].previous = kInvalid;
			else level.tail = kInvalid;
			order.previous = order.next = kInvalid;
			if (level.head == kInvalid) clear_occupied(order.side, level_index);
		}

		AlignedArray<Node> _nodes;
		AlignedArray<PriceLevel> _bids;
		AlignedArray<PriceLevel> _asks;
		AlignedArray<uint64_t> _bid_bitmap;
		AlignedArray<uint64_t> _ask_bitmap;
		FixedOrderIndex _index;
		std::size_t _order_capacity;
		int64_t _minimum_price;
		int64_t _tick_size;
		uint32_t _level_count;
		uint32_t _free;
		std::size_t _active;
		uint32_t _best_bid;
		uint32_t _best_ask;
	};
}

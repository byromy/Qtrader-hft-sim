#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>

namespace wthft
{
	enum class Side : uint8_t { Buy, Sell };
	enum class OrderType : uint8_t { Limit, Market };
	enum class OrderState : uint8_t { New, PartiallyFilled, Filled, Canceled, Rejected };

	struct OrderRequest
	{
		uint64_t id;
		Side side;
		OrderType type;
		int64_t price;
		uint64_t quantity;
	};

	struct Trade
	{
		uint64_t maker_id;
		uint64_t taker_id;
		int64_t price;
		uint64_t quantity;
	};

	struct OrderUpdate
	{
		uint64_t id;
		OrderState state;
		uint64_t remaining;
	};

	class PriceTimeOrderBook
	{
		struct Node;
		struct PriceLevel { Node* head = nullptr; Node* tail = nullptr; };
		struct Node
		{
			uint64_t id = 0;
			int64_t price = 0;
			uint64_t remaining = 0;
			Side side = Side::Buy;
			OrderState state = OrderState::New;
			Node* previous = nullptr;
			Node* next = nullptr;
			Node* free_next = nullptr;
		};

		using Bids = std::map<int64_t, PriceLevel, std::greater<int64_t>>;
		using Asks = std::map<int64_t, PriceLevel, std::less<int64_t>>;

	public:
		explicit PriceTimeOrderBook(std::size_t capacity)
			: _storage(new Node[capacity]), _capacity(capacity), _free(nullptr), _active(0)
		{
			_orders.reserve(capacity);
			for (std::size_t i = 0; i < capacity; ++i)
			{
				_storage[i].free_next = _free;
				_free = &_storage[i];
			}
		}

		template<typename Sink>
		bool submit(const OrderRequest& request, Sink& sink)
		{
			if (request.id == 0 || request.quantity == 0 ||
				(request.type == OrderType::Limit && request.price <= 0) ||
				_orders.find(request.id) != _orders.end())
			{
				sink.on_order(OrderUpdate{request.id, OrderState::Rejected, request.quantity});
				return false;
			}

			Node* order = allocate();
			if (order == nullptr)
			{
				sink.on_order(OrderUpdate{request.id, OrderState::Rejected, request.quantity});
				return false;
			}
			order->id = request.id;
			order->side = request.side;
			order->price = request.price;
			order->remaining = request.quantity;
			order->state = OrderState::New;
			_orders.emplace(order->id, order);

			match(*order, request.type, sink);
			if (order->remaining == 0)
			{
				order->state = OrderState::Filled;
				sink.on_order(OrderUpdate{order->id, order->state, 0});
				release(order);
				return true;
			}
			if (request.type == OrderType::Market)
			{
				order->state = OrderState::Canceled;
				sink.on_order(OrderUpdate{order->id, order->state, order->remaining});
				release(order);
				return true;
			}

			order->state = order->remaining == request.quantity ?
				OrderState::New : OrderState::PartiallyFilled;
			append(order);
			sink.on_order(OrderUpdate{order->id, order->state, order->remaining});
			return true;
		}

		template<typename Sink>
		bool cancel(uint64_t id, Sink& sink)
		{
			auto found = _orders.find(id);
			if (found == _orders.end())
				return false;
			Node* order = found->second;
			unlink(order);
			order->state = OrderState::Canceled;
			sink.on_order(OrderUpdate{id, order->state, order->remaining});
			release(order);
			return true;
		}

		template<typename Sink>
		bool modify(uint64_t id, int64_t new_price, uint64_t new_quantity, Sink& sink)
		{
			auto found = _orders.find(id);
			if (found == _orders.end() || new_price <= 0 || new_quantity == 0)
				return false;
			const Side side = found->second->side;
			unlink(found->second);
			release(found->second);
			return submit(OrderRequest{id, side, OrderType::Limit, new_price, new_quantity}, sink);
		}

		std::size_t active_orders() const { return _active; }
		int64_t best_bid() const { return _bids.empty() ? 0 : _bids.begin()->first; }
		int64_t best_ask() const { return _asks.empty() ? 0 : _asks.begin()->first; }

	private:
		Node* allocate()
		{
			if (_free == nullptr) return nullptr;
			Node* node = _free;
			_free = _free->free_next;
			*node = Node{};
			++_active;
			return node;
		}

		void release(Node* node)
		{
			_orders.erase(node->id);
			node->free_next = _free;
			_free = node;
			--_active;
		}

		void append(Node* order)
		{
			if (order->side == Side::Buy)
				append_to(_bids, order);
			else
				append_to(_asks, order);
		}

		template<typename Levels>
		void append_to(Levels& levels, Node* order)
		{
			PriceLevel& level = levels[order->price];
			order->previous = level.tail;
			if (level.tail) level.tail->next = order;
			else level.head = order;
			level.tail = order;
		}

		void unlink(Node* order)
		{
			if (order->side == Side::Buy)
				unlink_from(_bids, order);
			else
				unlink_from(_asks, order);
		}

		template<typename Levels>
		void unlink_from(Levels& levels, Node* order)
		{
			auto level_it = levels.find(order->price);
			if (level_it == levels.end()) return;
			PriceLevel& level = level_it->second;
			if (order->previous) order->previous->next = order->next;
			else level.head = order->next;
			if (order->next) order->next->previous = order->previous;
			else level.tail = order->previous;
			order->previous = order->next = nullptr;
			if (level.head == nullptr) levels.erase(level_it);
		}

		template<typename Sink>
		void match(Node& taker, OrderType type, Sink& sink)
		{
			if (taker.side == Side::Buy)
				match_levels(taker, type, _asks, sink);
			else
				match_levels(taker, type, _bids, sink);
		}

		template<typename Levels, typename Sink>
		void match_levels(Node& taker, OrderType type, Levels& levels, Sink& sink)
		{
			while (taker.remaining != 0 && !levels.empty())
			{
				auto level_it = levels.begin();
				const int64_t match_price = level_it->first;
				const bool crosses = type == OrderType::Market ||
					(taker.side == Side::Buy ? taker.price >= match_price : taker.price <= match_price);
				if (!crosses) break;

				PriceLevel& level = level_it->second;
				while (taker.remaining != 0 && level.head != nullptr)
				{
					Node* maker = level.head;
					const uint64_t quantity = taker.remaining < maker->remaining ?
						taker.remaining : maker->remaining;
					taker.remaining -= quantity;
					maker->remaining -= quantity;
					sink.on_trade(Trade{maker->id, taker.id, match_price, quantity});
					if (maker->remaining == 0)
					{
						maker->state = OrderState::Filled;
						sink.on_order(OrderUpdate{maker->id, maker->state, 0});
						unlink_from(levels, maker);
						release(maker);
						if (levels.empty() || levels.begin()->first != match_price) break;
					}
					else
					{
						maker->state = OrderState::PartiallyFilled;
						sink.on_order(OrderUpdate{maker->id, maker->state, maker->remaining});
					}
				}
			}
		}

		std::unique_ptr<Node[]> _storage;
		std::size_t _capacity;
		Node* _free;
		std::size_t _active;
		std::unordered_map<uint64_t, Node*> _orders;
		Bids _bids;
		Asks _asks;
	};
}

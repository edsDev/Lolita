#include "parsing-automaton.h"
#include "grammar.h"
#include "grammar-analytic.h"
#include <set>
#include <map>
#include <unordered_map>
#include <tuple>
#include <cassert>

using namespace std;
using namespace eds;

namespace eds::loli::parsing
{
	// Implmentation of ParsingAutomaton
	//


	// by convention, if production is set nullptr
	// it's an imaginary root production
	struct ParsingItem
	{
	public:
		Production* production;
		int cursor;

	public:
		ParsingItem(Production* p, int cursor)
			: production(p), cursor(cursor)
		{
			assert(p != nullptr);
			assert(cursor >= 0 && cursor <= p->rhs.size());
		}

		const Symbol* NextSymbol() const
		{
			if (IsFinalized()) return nullptr;

			return production->rhs[cursor];
		}

		// if it's not any P -> . \alpha
		// NOTE though production of root symbol is always kernal,
		// this struct is not aware of that
		bool IsKernal() const
		{
			return cursor > 0;
		}

		// if it's some P -> \alpha .
		bool IsFinalized() const
		{
			return cursor == production->rhs.size();
		}
	};

	// Operator Overloads for Item(for FlatSet)
	//

	inline bool operator==(ParsingItem lhs, ParsingItem rhs)
	{
		return lhs.production == rhs.production && lhs.cursor == rhs.cursor;
	}
	inline bool operator!=(ParsingItem lhs, ParsingItem rhs)
	{
		return !(lhs == rhs);
	}
	inline bool operator>(ParsingItem lhs, ParsingItem rhs)
	{
		if (lhs.production > rhs.production) return true;
		if (lhs.production < rhs.production) return false;

		return lhs.cursor > rhs.cursor;
	}
	inline bool operator>=(ParsingItem lhs, ParsingItem rhs)
	{
		return lhs == rhs || lhs > rhs;
	}
	inline bool operator<(ParsingItem lhs, ParsingItem rhs)
	{
		return !(lhs >= rhs);
	}
	inline bool operator<=(ParsingItem lhs, ParsingItem rhs)
	{
		return !(lhs > rhs);
	}

	using ItemSet = FlatSet<ParsingItem>;

	// Expands state of kernal items into its closure
	// that includes all of non-kernal items as well,
	// and then enumerate items with a callback
	void EnumerateClosureItems(const Grammar& g, const ItemSet& state, function<void(ParsingItem)> callback)
	{
		auto added = vector<bool>(g.NonterminalCount(), false);
		auto unvisited = vector<const Nonterminal*>{};

		// treat root symbol's items as kernal
		added[g.RootSymbol()->id] = true;

		// helper
		auto try_register_candidate = [&](const Symbol* s) {
			if (auto candidate = s->AsNonterminal(); candidate)
			{
				if (!added[candidate->id])
				{
					added[candidate->id] = true;
					unvisited.push_back(candidate);
				}
			}
		};

		// visit kernal items and record nonterms for closure calculation
		for (const auto& item : state)
		{
			callback(item);

			// Item{ A -> \alpha . }: skip it
			// Item{ A -> \alpha . B \gamma }: queue unvisited nonterm B for further expansion
			auto symbol = item.NextSymbol();
			if (symbol)
			{
				try_register_candidate(symbol);
			}
		}

		// genreate and visit non-kernal items recursively
		while (!unvisited.empty())
		{
			auto lhs = unvisited.back();
			unvisited.pop_back();

			for (const auto p : lhs->productions)
			{
				callback(ParsingItem{ p, 0 });

				const auto& rhs = p->rhs;
				if (!rhs.empty())
				{
					try_register_candidate(rhs.front());
				}
			}
		}
	}

	// TODO: directly generate vector
	// claculate target state from a source state with a particular symbol s
	// and enumerate its items with a callback
	ItemSet GenerateGotoItems(const Grammar& g, const ItemSet& src, const Symbol* s)
	{
		ItemSet new_state;

		EnumerateClosureItems(g, src, [&](ParsingItem item) {

			// ignore Item A -> \alpha .
			if (item.IsFinalized())
				return;

			// for Item A -> \alpha . B \beta where B == s
			// advance the cursor
			if (item.production->rhs[item.cursor] == s)
			{
				new_state.insert(ParsingItem{ item.production, item.cursor + 1 });
			}
		});

		return new_state;
	}

	ItemSet GenerateInitialItemSet(const Grammar& g)
	{
		ItemSet result;
		for (auto p : g.RootSymbol()->productions)
		{
			result.insert(ParsingItem{ p, 0 });
		}

		return result;
	}

	template <typename F>
	void EnumerateSymbols(const Grammar& g, F callback)
	{
		static_assert(is_invocable_v<F, const Symbol*>);

		for (const auto& term : g.Terminals())
			callback(&term);

		for (const auto& nonterm : g.Nonterminals())
			callback(&nonterm);
	}

	auto GenerateBasicParsingAutomaton(const Grammar& g)
	{
		// NOTE a set of ParsingItem coresponds to a ParsingState

		auto pda = make_unique<ParsingAutomaton>();
		auto initial_set = GenerateInitialItemSet(g);
		map<ItemSet, ParsingState*> state_lookup{ { initial_set, pda->NewState() } };

		// for each unvisited LR state, generate a target state for each possible symbol
		for (deque<ItemSet> unprocessed{ initial_set }; !unprocessed.empty(); unprocessed.pop_front())
		{
			const auto& src_item_set = unprocessed.front();
			const auto src_state = state_lookup[src_item_set];

			EnumerateSymbols(g, [&](const Symbol* s) {

				// calculate the target state for symbol s
				ItemSet dest_item_set = GenerateGotoItems(g, src_item_set, s);

				// empty set of item is not a valid state
				if (dest_item_set.empty()) return;

				// compute target state
				ParsingState* dest_state;
				auto iter = state_lookup.find(dest_item_set);
				if (iter != state_lookup.end())
				{
					dest_state = iter->second;
				}
				else
				{
					// if new_state is not recorded in states, create one
					dest_state = pda->NewState();

					// TODO: use move here?
					state_lookup.insert_or_assign(dest_item_set, dest_state);
					unprocessed.push_back(dest_item_set);
				}

				// add transition
				pda->RegisterShift(src_state, dest_state, s);
			});
		}

		return make_tuple(move(pda), move(state_lookup));
	}

	unique_ptr<const ParsingAutomaton> BuildSLRAutomaton(const Grammar& g)
	{
		auto[atm, state_lookup] = GenerateBasicParsingAutomaton(g);

		// calculate FIRST and FOLLOW set first
		const auto ps = ComputePredictiveSet(g);

		for (const auto& state_def : state_lookup)
		{
			const auto& item_set = get<0>(state_def);
			const auto& state = get<1>(state_def);

			for (auto item : item_set)
			{
				const auto& lhs = item.production->lhs;
				const auto& rhs = item.production->rhs;

				const auto& lhs_info = ps.at(lhs);

				if (item.IsFinalized())
				{
					// for all term in FOLLOW do reduce
					for (auto term : lhs_info.follow_set)
					{
						atm->RegisterReduce(state, item.production, term);
					}

					// EOF
					if (lhs_info.may_preceed_eof)
					{
						atm->RegisterReduceOnEof(state, item.production);
					}
				}
			}
		}

		return move(atm);
	}
}
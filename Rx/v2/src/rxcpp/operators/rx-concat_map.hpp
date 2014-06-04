// Copyright (c) Microsoft Open Technologies, Inc. All rights reserved. See License.txt in the project root for license information.

#pragma once

#if !defined(RXCPP_OPERATORS_RX_CONCATMAP_HPP)
#define RXCPP_OPERATORS_RX_CONCATMAP_HPP

#include "../rx-includes.hpp"

namespace rxcpp {

namespace operators {

namespace detail {

template<class Observable, class CollectionSelector, class ResultSelector, class Coordination>
struct concat_traits {
    typedef typename std::decay<Observable>::type source_type;
    typedef typename std::decay<CollectionSelector>::type collection_selector_type;
    typedef typename std::decay<ResultSelector>::type result_selector_type;
    typedef typename std::decay<Coordination>::type coordination_type;

    typedef typename source_type::value_type source_value_type;

    struct tag_not_valid {};
    template<class CV, class CCS>
    static auto collection_check(int) -> decltype((*(CCS*)nullptr)(*(CV*)nullptr));
    template<class CV, class CCS>
    static tag_not_valid collection_check(...);

    static_assert(!std::is_same<decltype(collection_check<source_value_type, collection_selector_type>(0)), tag_not_valid>::value, "concat_map CollectionSelector must be a function with the signature observable(concat_map::source_value_type)");

    typedef decltype((*(collection_selector_type*)nullptr)((*(source_value_type*)nullptr))) collection_type;

//#if _MSC_VER >= 1900
    static_assert(is_observable<collection_type>::value, "concat_map CollectionSelector must return an observable");
//#endif

    typedef typename collection_type::value_type collection_value_type;

    template<class CV, class CCV, class CRS>
    static auto result_check(int) -> decltype((*(CRS*)nullptr)(*(CV*)nullptr, *(CCV*)nullptr));
    template<class CV, class CCV, class CRS>
    static tag_not_valid result_check(...);

    static_assert(!std::is_same<decltype(result_check<source_value_type, collection_value_type, result_selector_type>(0)), tag_not_valid>::value, "concat_map ResultSelector must be a function with the signature concat_map::value_type(concat_map::source_value_type, concat_map::collection_value_type)");

    typedef decltype((*(result_selector_type*)nullptr)(*(source_value_type*)nullptr, *(collection_value_type*)nullptr)) value_type;
};

template<class Observable, class CollectionSelector, class ResultSelector, class Coordination>
struct concat_map
    : public operator_base<typename concat_traits<Observable, CollectionSelector, ResultSelector, Coordination>::value_type>
{
    typedef concat_map<Observable, CollectionSelector, ResultSelector, Coordination> this_type;
    typedef concat_traits<Observable, CollectionSelector, ResultSelector, Coordination> traits;

    typedef typename traits::source_type source_type;
    typedef typename traits::collection_selector_type collection_selector_type;
    typedef typename traits::result_selector_type result_selector_type;

    typedef typename traits::source_value_type source_value_type;
    typedef typename traits::collection_type collection_type;
    typedef typename traits::collection_value_type collection_value_type;

    typedef typename traits::coordination_type coordination_type;
    typedef typename coordination_type::coordinator_type coordinator_type;

    struct values
    {
        values(source_type o, collection_selector_type s, result_selector_type rs, coordination_type sf)
            : source(std::move(o))
            , selectCollection(std::move(s))
            , selectResult(std::move(rs))
            , coordination(std::move(sf))
        {
        }
        source_type source;
        collection_selector_type selectCollection;
        result_selector_type selectResult;
        coordination_type coordination;
    };
    values initial;

    concat_map(source_type o, collection_selector_type s, result_selector_type rs, coordination_type sf)
        : initial(std::move(o), std::move(s), std::move(rs), std::move(sf))
    {
    }

    template<class Subscriber>
    void on_subscribe(Subscriber scbr) const {
        static_assert(is_subscriber<Subscriber>::value, "subscribe must be passed a subscriber");

        typedef typename coordinator_type::template get<Subscriber>::type output_type;

        struct concat_map_state_type
            : public std::enable_shared_from_this<concat_map_state_type>
            , public values
        {
            concat_map_state_type(values i, coordinator_type coor, output_type oarg)
                : values(std::move(i))
                , sourceLifetime(composite_subscription::empty())
                , collectionLifetime(composite_subscription::empty())
                , coordinator(std::move(coor))
                , out(std::move(oarg))
            {
            }

            void subscribe_to(source_value_type st)
            {
                auto state = this->shared_from_this();

                auto selectedCollection = on_exception(
                    [&](){return state->selectCollection(st);},
                    state->out);
                if (selectedCollection.empty()) {
                    return;
                }

                collectionLifetime = composite_subscription();

                // when the out observer is unsubscribed all the
                // inner subscriptions are unsubscribed as well
                auto innercstoken = state->out.add(collectionLifetime);

                collectionLifetime.add(make_subscription([state, innercstoken](){
                    state->out.remove(innercstoken);
                }));

                auto selectedSource = on_exception(
                    [&](){return state->coordinator(selectedCollection.get());},
                    state->out);
                if (selectedSource.empty()) {
                    return;
                }

                // this subscribe does not share the source subscription
                // so that when it is unsubscribed the source will continue
                selectedSource->subscribe(
                    state->out,
                    collectionLifetime,
                // on_next
                    [state, st](collection_value_type ct) {
                        auto selectedResult = on_exception(
                            [&](){return state->selectResult(st, std::move(ct));},
                            state->out);
                        if (selectedResult.empty()) {
                            return;
                        }
                        state->out.on_next(std::move(*selectedResult));
                    },
                // on_error
                    [state](std::exception_ptr e) {
                        state->out.on_error(e);
                    },
                //on_completed
                    [state](){
                        if (!state->selectedCollections.empty()) {
                            auto value = state->selectedCollections.front();
                            state->selectedCollections.pop_front();
                            state->collectionLifetime.unsubscribe();
                            state->subscribe_to(value);
                        } else if (!state->sourceLifetime.is_subscribed()) {
                            state->out.on_completed();
                        }
                    }
                );
            }
            composite_subscription sourceLifetime;
            composite_subscription collectionLifetime;
            std::deque<source_value_type> selectedCollections;
            coordinator_type coordinator;
            output_type out;
        };

        auto coordinator = initial.coordination.create_coordinator();
        auto selectedDest = on_exception(
            [&](){return coordinator(scbr);},
            scbr);
        if (selectedDest.empty()) {
            return;
        }

        // take a copy of the values for each subscription
        auto state = std::shared_ptr<concat_map_state_type>(new concat_map_state_type(initial, std::move(coordinator), std::move(selectedDest.get())));

        state->sourceLifetime = composite_subscription();

        // when the out observer is unsubscribed all the
        // inner subscriptions are unsubscribed as well
        state->out.add(state->sourceLifetime);

        auto source = on_exception(
            [&](){return state->coordinator(state->source);},
            state->out);
        if (source.empty()) {
            return;
        }

        // this subscribe does not share the observer subscription
        // so that when it is unsubscribed the observer can be called
        // until the inner subscriptions have finished
        source->subscribe(
            state->out,
            state->sourceLifetime,
        // on_next
            [state](source_value_type st) {
                if (state->collectionLifetime.is_subscribed()) {
                    state->selectedCollections.push_back(st);
                } else if (state->selectedCollections.empty()) {
                    state->subscribe_to(st);
                }
            },
        // on_error
            [state](std::exception_ptr e) {
                state->out.on_error(e);
            },
        // on_completed
            [state]() {
                if (!state->collectionLifetime.is_subscribed() && state->selectedCollections.empty()) {
                    state->out.on_completed();
                }
            }
        );
    }
};

template<class CollectionSelector, class ResultSelector, class Coordination>
class concat_map_factory
{
    typedef typename std::decay<CollectionSelector>::type collection_selector_type;
    typedef typename std::decay<ResultSelector>::type result_selector_type;
    typedef typename std::decay<Coordination>::type coordination_type;

    collection_selector_type selectorCollection;
    result_selector_type selectorResult;
    coordination_type coordination;
public:
    concat_map_factory(collection_selector_type s, result_selector_type rs, coordination_type sf)
        : selectorCollection(std::move(rs))
        , selectorResult(std::move(s))
        , coordination(std::move(sf))
    {
    }

    template<class Observable>
    auto operator()(Observable&& source)
        ->      observable<typename concat_map<Observable, CollectionSelector, ResultSelector, Coordination>::value_type, concat_map<Observable, CollectionSelector, ResultSelector, Coordination>> {
        return  observable<typename concat_map<Observable, CollectionSelector, ResultSelector, Coordination>::value_type, concat_map<Observable, CollectionSelector, ResultSelector, Coordination>>(
                                    concat_map<Observable, CollectionSelector, ResultSelector, Coordination>(std::forward<Observable>(source), selectorCollection, selectorResult, coordination));
    }
};

}

template<class CollectionSelector, class ResultSelector, class Coordination>
auto concat_map(CollectionSelector&& s, ResultSelector&& rs, Coordination&& sf)
    ->      detail::concat_map_factory<CollectionSelector, ResultSelector, Coordination> {
    return  detail::concat_map_factory<CollectionSelector, ResultSelector, Coordination>(std::forward<CollectionSelector>(s), std::forward<ResultSelector>(rs), std::forward<Coordination>(sf));
}

}

}

#endif
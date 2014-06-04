// Copyright (c) Microsoft Open Technologies, Inc. All rights reserved. See License.txt in the project root for license information.

#pragma once

#if !defined(RXCPP_OPERATORS_RX_MERGE_HPP)
#define RXCPP_OPERATORS_RX_MERGE_HPP

#include "../rx-includes.hpp"

namespace rxcpp {

namespace operators {

namespace detail {

template<class Observable, class Coordination>
struct merge : public operator_base<typename std::decay<Observable>::type::value_type::value_type>
{
    typedef merge<Observable, Coordination> this_type;

    typedef typename std::decay<Observable>::type source_type;
    typedef typename source_type::value_type source_value_type;
    typedef typename source_value_type::value_type value_type;
    typedef typename std::decay<Coordination>::type coordination_type;
    typedef typename coordination_type::coordinator_type coordinator_type;

    struct values
    {
        values(source_type o, coordination_type sf)
            : source(std::move(o))
            , coordination(std::move(sf))
        {
        }
        source_type source;
        coordination_type coordination;
    };
    values initial;

    merge(source_type o, coordination_type sf)
        : initial(std::move(o), std::move(sf))
    {
    }

    template<class Subscriber>
    void on_subscribe(Subscriber scbr) const {
        static_assert(is_subscriber<Subscriber>::value, "subscribe must be passed a subscriber");

        typedef typename coordinator_type::template get<Subscriber>::type output_type;

        struct merge_state_type
            : public std::enable_shared_from_this<merge_state_type>
            , public values
        {
            merge_state_type(values i, coordinator_type coor, output_type oarg)
                : values(std::move(i))
                , pendingCompletions(0)
                , coordinator(std::move(coor))
                , out(std::move(oarg))
            {
            }
            // on_completed on the output must wait until all the
            // subscriptions have received on_completed
            int pendingCompletions;
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
        auto state = std::shared_ptr<merge_state_type>(new merge_state_type(initial, std::move(coordinator), std::forward<Subscriber>(selectedDest.get())));

        composite_subscription outercs;

        // when the out observer is unsubscribed all the
        // inner subscriptions are unsubscribed as well
        state->out.add(outercs);

        auto source = on_exception(
            [&](){return state->coordinator(state->source);},
            state->out);
        if (source.empty()) {
            return;
        }

        ++state->pendingCompletions;
        // this subscribe does not share the observer subscription
        // so that when it is unsubscribed the observer can be called
        // until the inner subscriptions have finished
        source->subscribe(
            state->out,
            outercs,
        // on_next
            [state](source_value_type st) {

                composite_subscription innercs;

                // when the out observer is unsubscribed all the
                // inner subscriptions are unsubscribed as well
                auto innercstoken = state->out.add(innercs);

                innercs.add(make_subscription([state, innercstoken](){
                    state->out.remove(innercstoken);
                }));

                auto selectedSource = on_exception(
                    [&](){return state->coordinator(st);},
                    state->out);
                if (selectedSource.empty()) {
                    return;
                }

                ++state->pendingCompletions;
                // this subscribe does not share the source subscription
                // so that when it is unsubscribed the source will continue
                selectedSource->subscribe(
                    state->out,
                    innercs,
                // on_next
                    [state, st](value_type ct) {
                        state->out.on_next(std::move(ct));
                    },
                // on_error
                    [state](std::exception_ptr e) {
                        state->out.on_error(e);
                    },
                //on_completed
                    [state](){
                        if (--state->pendingCompletions == 0) {
                            state->out.on_completed();
                        }
                    }
                );
            },
        // on_error
            [state](std::exception_ptr e) {
                state->out.on_error(e);
            },
        // on_completed
            [state]() {
                if (--state->pendingCompletions == 0) {
                    state->out.on_completed();
                }
            }
        );
    }
};

template<class Coordination>
class merge_factory
{
    typedef typename std::decay<Coordination>::type coordination_type;

    coordination_type coordination;
public:
    merge_factory(coordination_type sf)
        : coordination(std::move(sf))
    {
    }

    template<class Observable>
    auto operator()(Observable&& source)
        ->      observable<typename merge<Observable, Coordination>::value_type, merge<Observable, Coordination>> {
        return  observable<typename merge<Observable, Coordination>::value_type, merge<Observable, Coordination>>(
                                    merge<Observable, Coordination>(std::forward<Observable>(source), coordination));
    }
};

}

template<class Coordination>
auto merge(Coordination&& sf)
    ->      detail::merge_factory<Coordination> {
    return  detail::merge_factory<Coordination>(std::forward<Coordination>(sf));
}

}

}

#endif
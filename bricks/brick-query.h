// -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4 -*-
// vim: syntax=cpp tabstop=4 shiftwidth=4 expandtab

/*
 * Utilities for querying data collections uniformly
 */

/*
 * (c) 2014 Vladimír Štill <xstill@fi.muni.cz>
 */

/* Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE. */

#include <algorithm>
#include <numeric>
#include <type_traits>
#include <iterator>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <deque> // for tests

#include <brick-assert.h>

#ifndef BRICK_QUERY_H
#define BRICK_QUERY_H

namespace brick {
namespace query {

template< typename It >
struct _ValueType { using Type = typename It::value_type; };

template< typename T >
struct _ValueType< T * > { using Type = T; };

/* range; basically just a tuple of iterrators.
 * Use wrapper functions range/crange for construction.
 *
 * Itself behaves kind of like collection as it has begin()/end() and iterrators typedef.
 * Creating Range of Range gives range equivalent to original.
 */
template< typename InputIt >
struct Range {
    using Iterator = InputIt;
    using iterator = InputIt;
    using ValueType = typename _ValueType< InputIt >::Type;
    using value_type = ValueType;

    InputIt begin() { return _begin; }
    InputIt end() { return _end; }

    Range() = default;
    Range( const Range & ) = default;
    Range( Range && ) = default;
    Range( InputIt begin, InputIt end ) : _begin( begin ), _end( end ) { }

    template< typename Collection,
        typename = typename std::enable_if< std::is_same< decltype( std::declval< Collection & >().begin() ), InputIt >::value >::type >
    explicit Range( Collection &col ) : Range( col.begin(), col.end() ) { }

    template< typename Collection,
        typename = typename std::enable_if< std::is_same< decltype( std::declval< const Collection & >().begin() ), InputIt >::value >::type >
    explicit Range( const Collection &col ) : Range( col.begin(), col.end() ) { }

  private:
    InputIt _begin;
    InputIt _end;
};

template< typename Collection >
auto range( Collection &col ) -> Range< typename Collection::iterator > {
    return Range< typename Collection::iterator >( col );
}

template< typename Collection >
auto range( const Collection &col ) -> Range< typename Collection::const_iterator > {
    return Range< typename Collection::const_iterator >( col );
}

template< typename Collection >
auto crange( const Collection &col ) -> Range< typename Collection::const_iterator > {
    return Range< typename Collection::const_iterator >( col );
}

template< typename InputIt, typename UnaryPred >
bool all( InputIt first, InputIt last, UnaryPred pred ) {
    return std::all_of( first, last, pred );
}

template< typename Collection, typename UnaryPred >
bool all( const Collection &col, UnaryPred pred ) {
    return all( col.begin(), col.end(), pred );
}

template< typename InputIt, typename UnaryPred >
bool any( InputIt first, InputIt last, UnaryPred pred ) {
    return std::any_of( first, last, pred );
}

template< typename Collection, typename UnaryPred >
bool any( const Collection &col, UnaryPred pred ) {
    return any( col.begin(), col.end(), pred );
}

template< typename InputIt, typename UnaryPred >
bool none( InputIt first, InputIt last, UnaryPred pred ) {
    return std::none_of( first, last, pred );
}

template< typename Collection, typename UnaryPred >
bool none( const Collection &col, UnaryPred pred ) {
    return none( col.begin(), col.end(), pred );
}

template< typename Range, typename UnaryFn >
struct Map {
    Map() = default;
    Map( Range range, UnaryFn fn ) : _range( range ), _fn( fn ) { }

    using ValueType = typename std::result_of< UnaryFn( typename Range::iterator::value_type ) >::type;
    using BaseIterator = typename Range::iterator;

    struct Iterator : std::iterator< std::forward_iterator_tag, ValueType > {
        Iterator() : _map( nullptr ) { }
        Iterator( BaseIterator it, Map< Range, UnaryFn > *map ) :
            _it( it ), _map( map )
        { }

        friend bool operator==( const Iterator &a, const Iterator &b ) {
            return a._it == b._it;
        }
        friend bool operator!=( const Iterator &a, const Iterator &b ) {
            return !( a == b );
        }

        ValueType &operator*() { evaluate(); return *_current; }
        ValueType *operator->() { evaluate(); return _current.get(); }

        Iterator &operator++() {
            ++_it;
            _current = nullptr;
            return *this;
        }

        Iterator operator++( int ) {
            Iterator x = *this;
            ++(*this);
            return x;
        }

        void evaluate() {
            if ( !_current )
                _current = std::make_shared< ValueType >( _map->_fn( *_it ) );
        }

        bool end() const { return _it == _map->_range.end(); }

      private:
        BaseIterator _it;
        // it is kind of unfortunate to use shared_ptr here, but we have to
        // preserve instance accross copies, otherwise flatten (and possibly
        // other) will not work as nested collections/data structures would
        // be copyed.
        // This also makes Map lazy in value, which is good, but it would
        // be better if we could achieve this by Maybe/Optional.
        std::shared_ptr< ValueType > _current;
        Map< Range, UnaryFn > *_map;
    };

    Iterator begin() { return Iterator( _range.begin(), this ); }
    Iterator end() { return Iterator( _range.end(), this ); }

    using iterator = Iterator;
    using value_type = ValueType;

  private:
    Range _range;
    UnaryFn _fn;
};

template< typename Range, typename UnaryPred >
struct Filter {

    using ValueType = typename Range::iterator::value_type;
    using BaseIterator = typename Range::iterator;

    Filter() = default;
    Filter( Range range, UnaryPred pred ) : _range( range ), _pred( pred ) { }

    struct Iterator : std::iterator< std::forward_iterator_tag, ValueType > {
        Iterator() : _filter( nullptr ) { }
        Iterator( BaseIterator it, Filter< Range, UnaryPred > *filter ) :
            _it( it ), _filter( filter )
        {
            _bump();
        }

        friend bool operator==( const Iterator &a, const Iterator &b ) {
            return a._it == b._it;
        }
        friend bool operator!=( const Iterator &a, const Iterator &b ) {
            return !( a == b );
        }

        ValueType &operator*() { return *_it; }
        ValueType *operator->() { return &*_it; }

        Iterator &operator++() {
            ++_it;
            _bump();
            return *this;
        }

        Iterator operator++( int ) {
            Iterator x = *this;
            ++(*this);
            return x;
        }

        bool end() const { return _it == _filter->_range.end(); }

      private:
        BaseIterator _it;
        Filter< Range, UnaryPred > *_filter;

        void _bump() {
            while ( !end() && !_filter->_pred( *_it ) )
                ++_it;
        }
    };

    Iterator begin() { return Iterator( _range.begin(), this ); }
    Iterator end() { return Iterator( _range.end(), this ); }

    using iterator = Iterator;
    using value_type = ValueType;

  private:
    Range _range;
    UnaryPred _pred;
};

template< typename Range >
struct Flatten {

    using BaseIterator = typename Range::iterator;
    using SubIterator = typename Range::value_type::iterator;
    using ValueType = typename _ValueType< SubIterator >::Type;

    Flatten() = default;
    Flatten( Range range ) : _range( range ) { }

    struct Iterator : std::iterator< std::forward_iterator_tag, ValueType > {
        Iterator() : _flatten( nullptr ) { }
        Iterator( BaseIterator it, Flatten< Range > *flatten ) :
            _it( it ), _flatten( flatten )
        {
            if ( !end() ) {
                _sub = _it->begin();
                _bump();
            }
        }

        friend bool operator==( const Iterator &a, const Iterator &b ) {
            return ( a.end() && b.end() ) || ( a._it == b._it && a._sub == b._sub );
        }
        friend bool operator !=( const Iterator &a, const Iterator &b ) {
            return !( a == b );
        }

        ValueType &operator*() { return *_sub; }
        ValueType *operator->() { return &*_sub; }

        Iterator &operator++() {
            ++_sub;
            _bump();
            return *this;
        }

        Iterator operator++( int ) {
            Iterator x = *this;
            ++(*this);
            return x;
        }

        bool end() const { return _it == _flatten->_range.end(); }

      private:
        BaseIterator _it;
        SubIterator _sub;
        Flatten< Range > *_flatten;

        void _bump() {
            while ( !end() && _sub == _it->end() ) {
                ++_it;
                if ( !end() )
                    _sub = _it->begin();
            }
        }
    };

    Iterator begin() { return Iterator( _range.begin(), this ); }
    Iterator end() { return Iterator( _range.end(), this ); }

    using iterator = Iterator;
    using value_type = ValueType;

  private:
    Range _range;
};

template< typename Range >
struct Query {
    Query() = default;
    Query( const Query & ) = default;
    Query( Query && ) = default;

    template< typename... Args >
    Query( Args &&... args ) : _range( std::forward< Args >( args )... ) { }

    using Iterator = typename Range::iterator;
    using iterator = Iterator;
    using ValueType = typename Range::value_type;
    using value_type = ValueType;

    template< typename UnaryPred >
    bool all( UnaryPred pred ) {
        return query::all( _range, pred );
    }

    template< typename UnaryPred >
    bool any( UnaryPred pred ) {
        return query::any( _range, pred );
    }

    template< typename UnaryPred >
    bool none( UnaryPred pred ) {
        return query::none( _range, pred );
    }

    template< typename UnaryFn >
    auto map( UnaryFn fn ) -> Query< Map< Range, UnaryFn > > {
        return Query< Map< Range, UnaryFn > >( _range, fn );
    }

    template< typename UnaryPred >
    auto filter( UnaryPred pred ) -> Query< Filter< Range, UnaryPred > > {
        return Query< Filter< Range, UnaryPred > >( _range, pred );
    }

    auto flatten() -> Query< Flatten< Range > > {
        return Query< Flatten< Range > >( _range );
    }

    template< typename UnaryFn >
    auto concatMap( UnaryFn fn ) -> Query< Flatten< Map< Range, UnaryFn > > > {
        return map( fn ).flatten();
    }

    auto freeze() -> std::vector< ValueType > {
        return freezeAs< std::vector< ValueType > >();
    }

    template< typename Target >
    auto freezeAs() -> Target {
        return Target( _range.begin(), _range.end() );
    }

    ptrdiff_t size() { return std::distance( begin(), end() ); }

    template< typename UnaryFn >
    void forall( UnaryFn fn ) {
        for ( auto &a : _range )
            fn( a );
    }

    template< typename KeySelect,
        typename Key = typename std::result_of< KeySelect( ValueType & ) >::type >
    auto groupBy( KeySelect fn ) -> Query< std::map< Key, std::vector< ValueType > > >
    {
        std::map< Key, std::vector< ValueType > > map;
        for ( auto &a : _range )
            map[ fn( a ) ].push_back( a );
        return Query< std::map< Key, std::vector< ValueType > > >( std::move( map ) );
    }

    template< typename T, typename BinaryOperation >
    T fold( T init, BinaryOperation op ) {
        return std::accumulate( begin(), end(), init, op );
    }

    ValueType minOr( ValueType x ) {
        auto it = std::min_element( begin(), end() );
        if ( it == end() )
            return x;
        return *it;
    }

    ValueType maxOr( ValueType x ) {
        auto it = std::max_element( begin(), end() );
        if ( it == end() )
            return x;
        return *it;
    }

    auto sort() -> Query< std::set< ValueType > > {
        return Query< std::set< ValueType > >( begin(), end() );
    }

    ValueType median() {
        auto sorted = sort();
        auto la = sorted.begin(),
             ib = la++,
             ie = sorted.end();
        --ie;
        while ( ib != ie && la != ie ) {
            --ie;
            ib = la++;
        }
        if ( ib == ie )
            return *ib;
        return (*ib + *ie) / ValueType( 2 );
    }

    ValueType average() {
        ValueType sum;
        ptrdiff_t size;
        std::tie( sum, size ) = fold( { ValueType(), 0 },
                []( std::tuple< ValueType, ptrdiff_t > acc, const ValueType &val ) {
                    return std::make_tuple( std::get< 0 >( acc ) + val, std::get< 1 >( acc ) + 1 );
                } );
        return sum / ValueType( size );
    }

    ValueType min() { return *std::min_element( begin(), end() ); }
    ValueType max() { return *std::max_element( begin(), end() ); }

    Iterator begin() { return _range.begin(); }
    Iterator end() { return _range.end(); }

  private:
    Range _range;
};

template< typename Collection >
auto query( Collection &col ) -> Query< Range< typename Collection::iterator > > {
    return Query< Range< typename Collection::iterator > >( range( col ) );
}

template< typename Collection >
auto query( const Collection &col ) -> Query< Range< typename Collection::const_iterator > > {
    return Query< Range< typename Collection::const_iterator > >( crange( col ) );
}

template< typename Collection >
auto cquery( const Collection &col ) -> Query< Range< typename Collection::const_iterator > > {
    return Query< Range< typename Collection::const_iterator > >( crange( col ) );
}

template< typename Collection >
auto owningQuery( Collection &&col )
    -> Query< typename std::remove_reference< Collection >::type >
{
    return Query< typename std::remove_reference< Collection >::type >(
            std::forward< Collection >( col ) );
}


}
}

namespace brick_test {
namespace query {

struct Range {
    TEST(range) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        auto range = brick::query::range( vec );
        static_assert( std::is_same< int, decltype( range )::iterator::value_type >::value, "types" );
        auto rit = range.begin();
        for ( auto i : vec ) {
            ASSERT_EQ( i, *rit );
            ++rit;
        }
        auto vit = vec.begin();
        for ( auto i : range ) {
            ASSERT_EQ( i, *vit );
            ++vit;
        }
    }
};

struct Query {

    struct Id { template< typename T > T operator()( T t ) const { return t; } };
    struct ConstTrue { template< typename T > bool operator()( T t ) const { return true; } };
    struct ConstFalse { template< typename T > bool operator()( T t ) const { return false; } };

    static const Id id;
    static const ConstTrue constTrue;
    static const ConstFalse constFalse;

    TEST(query) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        auto query = brick::query::query( vec );
        static_assert( std::is_same< int, decltype( query )::iterator::value_type >::value, "types" );
        auto qit = query.begin();
        for ( auto i : vec ) {
            ASSERT_EQ( i, *qit );
            ++qit;
        }
        auto vit = vec.begin();
        for ( auto i : query ) {
            ASSERT_EQ( i, *vit );
            ++vit;
        }
    }

    // freeze . query == id
    TEST(freeze_id) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        ASSERT( brick::query::query( vec ).freeze() == vec );
    }

    TEST(freeze_id_deque) {
        std::deque< int > deq = { 1, 2, 3, 4 };
        ASSERT( brick::query::query( deq ).freezeAs< std::deque< int > >() == deq );
    }

    // freeze . map( id ) . query == id
    TEST(map_id) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        ASSERT( brick::query::query( vec ).map( id ).freeze() == vec );
    }

    TEST(map_it) {
        std::vector< int > vec = { 1, 2 };
        auto q = brick::query::query( vec ).map( id );
        ASSERT( !q.begin().end() );
        ASSERT( q.end().end() );
        ASSERT( !(++q.begin()).end() );
        ASSERT( (++(++q.begin())).end() );
    }

    TEST(map) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        std::vector< int > doubles = { 2, 4, 6, 8 };
        ASSERT( brick::query::query( vec ).map( []( int x ) { return x * 2; } ).freeze() == doubles );
    }

    // freeze . filter( const_true ) . query == id
    TEST(filter_id) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        ASSERT( brick::query::query( vec ).filter( constTrue ).freeze() == vec );
    }

    TEST(filter_it) {
        std::vector< int > vec = { 1, 2 };
        auto q = brick::query::query( vec ).filter( constTrue );
        ASSERT( !q.begin().end() );
        ASSERT( q.end().end() );
        ASSERT( !(++q.begin()).end() );
        ASSERT( (++(++q.begin())).end() );
    }

    TEST(filter) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        std::vector< int > odd = { 1, 3 };
        ASSERT( brick::query::query( vec ).filter( []( int x ) { return x % 2 == 1; } ).freeze() == odd );
        std::vector< int > empty = { };
        ASSERT( brick::query::query( vec ).filter( constFalse ).freeze() == empty );
    }

    TEST(flatten_it) {
        std::vector< std::vector< int > > vec = { { }, { 1 }, { }, { }, { 2 }, { }, { } };
        auto q = brick::query::query( vec ).flatten();
        ASSERT( !q.begin().end() );
        ASSERT( q.end().end() );
        ASSERT( !(++q.begin()).end() );
        ASSERT( (++(++q.begin())).end() );
    }

    TEST(flatten_map_it) {
        std::vector< std::vector< int > > vec = { };
        ASSERT( brick::query::query( vec ).map( id ).flatten().begin().end() );

        vec = { { }, { 1 }, { }, { }, { 2 }, { }, { } };
        auto q = brick::query::query( vec ).map( id ).flatten();

        ASSERT( !q.begin().end() );
        ASSERT( q.end().end() );
        ASSERT( !(++q.begin()).end() );
        ASSERT( (++(++q.begin())).end() );
    }

    TEST(flatten) {
        std::vector< std::vector< int > > vec = { { }, { }, { 1, 2 }, { 3 }, { }, { }, { }, { 4 }, { }, { }, { }, { } };
        std::vector< int > target = { 1, 2, 3, 4 };

        ASSERT_EQ( brick::query::query( vec ).flatten().size(), 4 );
        ASSERT( brick::query::query( vec ).flatten().freeze() == target );
    }

    TEST(size) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        ASSERT_EQ( brick::query::query( vec ).size(), 4 );
        ASSERT_EQ( brick::query::query( vec ).map( id ).size(), 4 );
        ASSERT_EQ( brick::query::query( vec ).filter( constTrue ).size(), 4 );
        ASSERT_EQ( brick::query::query( vec ).filter( constFalse ).size(), 0 );
        ASSERT_EQ( brick::query::query( vec )
                .filter( []( int x ) { return x % 2 == 0; } ).size(), 2 );

        std::vector< std::vector< int > > dvec = { { 1 } };
        ASSERT_EQ( brick::query::query( dvec ).map( id ).flatten().size(), 1 );
        ASSERT_EQ( brick::query::query( dvec ).flatten().map( id ).size(), 1 );

        dvec = { { 1 }, { 2, 3 }, { }, { 4 } };
        ASSERT_EQ( brick::query::query( dvec ).map( id ).flatten().size(), 4 );
        ASSERT_EQ( brick::query::query( dvec ).flatten().map( id ).size(), 4 );
    }

    TEST(map_flatten) {
        std::vector< std::deque< int > > vec = { { }, { }, { 1, 2 }, { 3 }, { }, { }, { }, { 4 }, { }, { }, { }, { } };
        std::vector< int > target = { 1, 2, 3, 4 };

        ASSERT_EQ( brick::query::query( vec ).map( id ).flatten().size(), 4 );
        ASSERT( brick::query::query( vec ).map( id ).flatten().freeze() == target );
    }

    TEST(filter_flatten) {
        std::vector< std::deque< int > > vec = { { }, { }, { 1, 2 }, { 3 }, { }, { }, { }, { 4 }, { }, { }, { }, { } };
        std::vector< int > target = { 1, 2 };

        ASSERT( brick::query::query( vec ).filter( []( std::deque< int > x ) { return x.size() % 2 == 0; } )
                .flatten().freeze() == target );
    }

    TEST(flatten_flatten) {
        std::vector< std::deque< std::vector< int > > > vec = { { }, { { }, { } }, { { 1, 2 } }, { { 3 }, { } }, { { 4 } }, };
        std::vector< int > target = { 1, 2, 3, 4 };

        ASSERT( brick::query::query( vec ).flatten().flatten().freeze() == target );
    }

    TEST(complicated) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        std::string str = brick::query::query( vec )
            .map( []( int x ) -> std::deque< int > {
                    std::deque< int > d;
                    for ( int i = 0; i < x; ++i )
                        d.push_back( i );
                    return d;
                    } )
            .flatten()
            .filter( []( int x ) { return x % 2 == 0; } )
            .concatMap( []( int x ) { return  x ? std::string( "" ) : std::string( "aa" ); } )
            .freezeAs< std::string >();
    }

    TEST(forall) {
        std::vector< int > vec = { 1, 2, 3, 4 };
        int sum = 0;
        brick::query::query( vec ).forall( [&]( int i ) { sum += i; } );
        ASSERT_EQ( sum, 10 );
    }

    TEST(groupBy) {
        std::vector< int > vec = { 1, 2, 2, 3, 3, 3, 4, 4, 4, 4 };
        brick::query::query( vec )
            .groupBy( id )
            .forall( []( std::pair< int, std::vector< int > > p ) {
                    ASSERT_EQ( p.second.size(), p.first );
                    for ( auto x : p.second )
                        ASSERT_EQ( x, p.first );
                } );
    }

};

}
}

#endif // BRICK_QUERY_H

// -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4 -*-

/*
 * Utilities and data structures for bit-level manipulation and data packing.
 */

/*
 * (c) 2013-2014 Jiří Weiser <xweiser1@fi.muni.cz>
 * (c) 2013 Petr Ročkai <me@mornfall.net>
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

#include <brick-assert.h>

#include <type_traits>

#ifdef __linux
#include <asm/byteorder.h>
#include <byteswap.h>
#elif !defined LITTLE_ENDIAN // if defined _WIN32
#define BYTE_ORDER 1234
#define LITTLE_ENDIAN 1234
#endif

#ifndef bswap_64
#define bswap_64 __builtin_bswap64
#endif

#include <atomic>
#include <cstring>

#ifndef BRICK_BITLEVEL_H
#define BRICK_BITLEVEL_H

namespace brick {
namespace bitlevel {

template< typename T1, typename T2 >
constexpr inline T1 align( T1 v, T2 a ) {
    return (v % T1(a)) ? (v + T1(a) - (v % T1(a))) : v;
}

template< typename T1, typename T2 >
constexpr inline T1 downalign( T1 v, T2 a ) {
    return v - (v % T1(a));
}

namespace compiletime {

template< typename T >
constexpr unsigned MSB( T x ) {
    return x > 1 ? 1 + MSB( x >> 1 ) : 0;
}

template< typename T >
constexpr T fill( T x ) {
    return x ? x | fill( x >> 1 ) : x;
}

template< typename T >
constexpr size_t sizeOf() {
    return std::is_empty< T >::value ? 0 : sizeof( T );
}

}

/*
 *  Fills `x` by bits up to the most si significant bit.
 *  Comlexity is O(log n), n is sizeof(x)*8
 */
template< typename number >
static inline number fill( number x ) {
    static const unsigned m = sizeof( number ) * 8;
    unsigned r = 1;
    if ( !x )
        return 0;
    while ( m != r ) {
        x |= x >> r;
        r <<= 1;
    }
    return x;
}

// get index of Most Significant Bit
// templated by argument to int, long, long long (all unsigned)
template< typename T >
static inline unsigned MSB( T x ) {
    unsigned position = 0;
    while ( x ) {
        x >>= 1;
        ++position;
    }
    return position - 1;
}

template<>
inline unsigned MSB< unsigned int >( unsigned int x ) {
    static const unsigned long bits = sizeof( unsigned int ) * 8 - 1;
    return bits - __builtin_clz( x );
}

template<>
inline unsigned MSB< unsigned long >( unsigned long x ) {
    static const unsigned bits = sizeof( unsigned long ) * 8 - 1;
    return bits - __builtin_clzl( x );
}

template<>
inline unsigned MSB< unsigned long long >( unsigned long long x ) {
    static const unsigned bits = sizeof( unsigned long long ) * 8 - 1;
    return bits - __builtin_clzll( x );
}

// gets only Most Significant Bit
template< typename number >
static inline number onlyMSB( number x ) {
    return number(1) << MSB( x );
}

// gets number without Most Significant Bit
template< typename number >
static inline number withoutMSB( number x ) {
    return x & ~onlyMSB( x );
}

inline uint64_t bitshift( uint64_t t, int shift ) {
#if BYTE_ORDER == LITTLE_ENDIAN
    return bswap_64( shift < 0 ? bswap_64( t << -shift ) : bswap_64( t >> shift ) );
#else
    return shift < 0 ? ( t << -shift ) : ( t >> shift );
#endif
}

struct BitPointer {
    BitPointer() : base( nullptr ), _bitoffset( 0 ) {}
    template< typename T > BitPointer( T *t, int offset = 0 )
        : base( static_cast< void * >( t ) ), _bitoffset( offset )
    {
        normalize();
    }
    uint32_t &word() { ASSERT( valid() ); return *static_cast< uint32_t * >( base ); }
    uint64_t &dword() { ASSERT( valid() ); return *static_cast< uint64_t * >( base ); }
    void normalize() {
        int shift = downalign( _bitoffset, 32 );
        _bitoffset -= shift;
        ASSERT_EQ( shift % 8, 0 );
        base = static_cast< uint32_t * >( base ) + shift / 32;
    }
    void shift( int bits ) { _bitoffset += bits; normalize(); }
    void fromReference( BitPointer r ) { *this = r; }
    int bitoffset() { return _bitoffset; }
    bool valid() { return base; }
private:
    void *base;
    int _bitoffset;
};

inline uint64_t mask( int first, int count ) {
    return bitshift(uint64_t(-1), -first) & bitshift(uint64_t(-1), (64 - first - count));
}

/*
 * NB. This function will alias whatever "to" points to with an uint64_t. With
 * aggressive optimisations, this might break code that passes an address of a
 * variable of different type. When "to" points to a stack variable, take
 * precautions to avoid breaking strict aliasing rules (the violation is not
 * detected by GCC as of 4.7.3).
 */

inline void bitcopy( BitPointer from, BitPointer to, int bitcount )
{
    while ( bitcount ) {
        int w = std::min( 32 - from.bitoffset(), bitcount );
        uint32_t fmask = mask( from.bitoffset(), w );
        uint64_t tmask = mask( to.bitoffset(), w );
        uint64_t bits = bitshift( from.word() & fmask, from.bitoffset() - to.bitoffset() );
        ASSERT_EQ( bits & ~tmask, 0u );
        ASSERT_EQ( bits & tmask, bits );
        if ( to.bitoffset() + bitcount > 32 )
            to.dword() = (to.dword() & ~tmask) | bits;
        else
            to.word() = (to.word() & ~static_cast< uint32_t >( tmask )) | static_cast< uint32_t >( bits );
        from.shift( w ); to.shift( w ); bitcount -= w; // slide
    }
}

template< typename T, int width = sizeof( T ) * 8 >
struct BitField
{
    static const int bitwidth = width;
    struct Virtual : BitPointer {
        void set( T t ) { bitcopy( BitPointer( &t ), *this, bitwidth ); }
        Virtual operator=( T t ) {
            set( t );
            return *this;
        }
        Virtual operator=( Virtual v ) {
            set( v.get() );
            return *this;
        }

        operator T() const { return get(); }
        T get() const {
            union U {
                uint64_t x;
                T t;
                U() : t() { }
            } u;
            bitcopy( *this, BitPointer( &u.x ), bitwidth );
            return u.t;
        }

        Virtual &operator++() {
            T value( get() );
            set( ++value );
            return *this;
        }
        T operator++(int) {
            T value( get() );
            T result( value++ );
            set( value );
            return result;
        }

        Virtual &operator--() {
            T value( get() );
            set( --value );
            return *this;
        }
        T operator--(int) {
            T value( get() );
            T result( value-- );
            set( value );
            return result;
        }
        template< typename U >
        Virtual operator+=( U value ) {
            T t( get() );
            t += value;
            set( t );
            return *this;
        }
        template< typename U >
        Virtual operator-=( U value ) {
            T t( get() );
            t -= value;
            set( t );
            return *this;
        }
        template< typename U >
        Virtual operator*=( U value ) {
            T t( get() );
            t *= value;
            set( t );
            return *this;
        }
        template< typename U >
        Virtual operator/=( U value ) {
            T t( get() );
            t /= value;
            set( t );
            return *this;
        }
        template< typename U >
        Virtual operator%=( U value ) {
            T t( get() );
            t %= value;
            set( t );
            return *this;
        }
    };
};

struct BitLock
{
    static const int bitwidth = 1;
    struct Virtual : BitPointer {
        using Atomic = std::atomic< uint32_t >;
        Atomic &atomic() { return *reinterpret_cast< Atomic * >( &word() ); }
        uint32_t bit() {
            ASSERT_LEQ( bitoffset(), 31 );
            return uint32_t( 1 ) << bitoffset();
        }
        void lock() {
            uint32_t l = word();
            do { l &= ~bit(); } while ( !atomic().compare_exchange_weak( l, l | bit() ) );
        }
        void unlock() { atomic().exchange( word() & ~bit() ); }
        bool locked() { return atomic().load() & bit(); }
    };
};

template< int O, typename... Args > struct BitAccess;

template< int O >
struct BitAccess< O > { static const int total = 0; };

template< int O, typename T, typename... Args >
struct BitAccess< O, T, Args... > {
    static const int offset = O;
    static const int width = T::bitwidth;
    typedef typename T::Virtual Head;
    typedef BitAccess< offset + T::bitwidth, Args... > Tail;
    static const int total = width + Tail::total;
};

template< typename BA, int I >
struct _AccessAt : _AccessAt< typename BA::Tail, I - 1 > {};

template< typename BA >
struct _AccessAt< BA, 0 > { using T = BA; };

template< typename... Args >
struct _BitTuple
{
    using Access = BitAccess< 0, Args... >;
    static const int bitwidth = Access::total;
    template< int I > using AccessAt = _AccessAt< Access, I >;
    template< int I > static int offset() { return AccessAt< I >::T::offset; }
};

template< typename... Args > struct BitTuple : _BitTuple< Args... >
{
    struct Virtual : BitPointer, _BitTuple< Args... > {};
    char storage[ align( Virtual::bitwidth, 32 ) / 8 ];
    BitTuple() { std::fill( storage, storage + sizeof( storage ), 0 ); }
    operator BitPointer() { return BitPointer( storage ); }
};

template< int I, typename BT >
typename BT::template AccessAt< I >::T::Head get( BT &bt )
{
    typename BT::template AccessAt< I >::T::Head t;
    t.fromReference( bt );
    t.shift( BT::template offset< I >() );
    return t;
}

}
}

namespace brick_test {
namespace bitlevel {

using namespace ::brick::bitlevel;

struct BitTupleTest {
    using U10 = BitField< unsigned, 10 >;
    using T10_10 = BitTuple< U10, U10 >;

    int bitcount( uint32_t word ) {
        int i = 0;
        while ( word ) {
            if ( word & 1 )
                ++i;
            word >>= 1;
        }
        return i;
    }

    TEST(mask) {
        /* only works on little endian machines ... */
        ASSERT_EQ( 0xFF00u, bitlevel::mask( 8, 8 ) );
        ASSERT_EQ( 0xF000u, bitlevel::mask( 12, 4 ) );
        ASSERT_EQ( 0x0F00u, bitlevel::mask( 8, 4 ) );
        ASSERT_EQ( 60u, bitlevel::mask( 2, 4 ) );// 0b111100
        ASSERT_EQ( 28u, bitlevel::mask( 2, 3 ) );// 0b11100
    }

    TEST(bitcopy) {
        uint32_t a = 42, b = 11;
        bitlevel::bitcopy( BitPointer( &a ), BitPointer( &b ), 32 );
        ASSERT_EQ( a, b );
        a = 0xFF00;
        bitlevel::bitcopy( BitPointer( &a ), BitPointer( &b, 8 ), 24 );
        ASSERT_EQ( b, 0xFF0000u | 42u );
        a = 0;
        bitlevel::bitcopy( BitPointer( &b, 8 ), BitPointer( &a ), 24 );
        ASSERT_EQ( a, 0xFF00u );
        bitlevel::bitcopy( BitPointer( &a, 8 ), BitPointer( &b, 8 ), 8 );

        a = 0x3FF;
        b = 0;
        bitlevel::bitcopy( BitPointer( &a, 0 ), BitPointer( &b, 0 ), 10 );
        ASSERT_EQ( b, 0x3FFu );

        unsigned char from[32], to[32];
        std::memset( from, 0, 32 );
        std::memset( to, 0, 32 );
        from[0] = 1 << 7;
        bitlevel::bitcopy( BitPointer( from, 7 ), BitPointer( to, 7 ), 1 );
        ASSERT_EQ( int( to[0] ), int( from[ 0 ] ) );
        from[0] = 1;
        to[0] = 0;
        bitlevel::bitcopy( BitPointer( from, 0 ), BitPointer( to, 7 ), 1 );
        ASSERT_EQ( int( to[0] ), 1 << 7 );

        from[0] = 13;
        from[1] = 63;
        bitlevel::bitcopy( BitPointer( from, 0 ), BitPointer( to, 32 ), 16 );
        ASSERT_EQ( int( to[4] ), int( from[0] ) );
        ASSERT_EQ( int( to[5] ), int( from[1] ) );

        from[0] = 2;
        from[1] = 2;
        std::memset( to, 0, 32 );
        bitlevel::bitcopy( BitPointer( from, 1 ), BitPointer( to, 32 ), 16 );
        ASSERT_EQ( int( to[4] ), 1 );
        ASSERT_EQ( int( to[5] ), 1 );

        from[0] = 1;
        from[1] = 1;
        std::memset( to, 0, 32 );
        bitlevel::bitcopy( BitPointer( from, 0 ), BitPointer( to, 33 ), 16 );
        ASSERT_EQ( int( to[4] ), 2 );
        ASSERT_EQ( int( to[5] ), 2 );

        from[0] = 1;
        from[1] = 1;
        std::memset( to, 0, 32 );
        for ( int i = 0; i < 16; ++i )
            bitlevel::bitcopy( BitPointer( from, i ), BitPointer( to, 33 + i ), 1 );
        ASSERT_EQ( int( to[4] ), 2 );
        ASSERT_EQ( int( to[5] ), 2 );

        for ( int i = 0; i < 16; ++i )
            from[i] = 2;
        std::memset( to, 0, 32 );
        bitlevel::bitcopy( BitPointer( from, 1 ), BitPointer( to, 3 ), 128 );
        for ( int i = 0; i < 16; ++i )
            ASSERT_EQ( int( to[i] ), 8 );
    }

    TEST(field) {
        int a = 42, b = 0;
        typedef BitField< int, 10 > F;
        F::Virtual f;
        f.fromReference( BitPointer( &b ) );
        f.set( a );
        ASSERT_EQ( a, 42 );
        ASSERT_EQ( a, f );
    }

    TEST(basic) {
        T10_10 x;
        ASSERT_EQ( T10_10::bitwidth, 20 );
        ASSERT_EQ( T10_10::offset< 0 >(), 0 );
        ASSERT_EQ( T10_10::offset< 1 >(), 10 );
        auto a = get< 0 >( x );
        auto b = get< 1 >( x );
        a.set( 5 );
        b.set( 7 );
        ASSERT_EQ( a, 5u );
        ASSERT_EQ( b, 7u );
    }

    TEST(big) {
        bitlevel::BitTuple< BitField< uint64_t, 63 >, BitField< uint64_t, 63 > > x;
        ASSERT_EQ( x.bitwidth, 126 );
        ASSERT_EQ( x.offset< 0 >(), 0 );
        ASSERT_EQ( x.offset< 1 >(), 63 );
        get< 0 >( x ).set( (1ull << 62) + 7 );
        ASSERT_EQ( get< 0 >( x ), (1ull << 62) + 7 );
        ASSERT_EQ( get< 1 >( x ), 0u );
        get< 0 >( x ).set( 0 );
        get< 1 >( x ).set( (1ull << 62) + 7 );
        ASSERT_EQ( get< 0 >( x ), 0u );
        ASSERT_EQ( get< 1 >( x ), (1ull << 62) + 7 );
        get< 0 >( x ).set( (1ull << 62) + 11 );
        ASSERT_EQ( get< 0 >( x ), (1ull << 62) + 11 );
        ASSERT_EQ( get< 1 >( x ), (1ull << 62) + 7 );
    }

    TEST(structure) {
        bitlevel::BitTuple< BitField< std::pair< uint64_t, uint64_t >, 120 >, BitField< uint64_t, 63 > > x;
        auto v = std::make_pair( (uint64_t( 1 ) << 62) + 7, uint64_t( 33 ) );
        ASSERT_EQ( x.bitwidth, 183 );
        ASSERT_EQ( x.offset< 0 >(), 0 );
        ASSERT_EQ( x.offset< 1 >(), 120 );
        get< 1 >( x ).set( 333 );
        ASSERT_EQ( get< 1 >( x ), 333u );
        get< 0 >( x ).set( v );
        ASSERT_EQ( get< 1 >( x ), 333u );
        ASSERT( get< 0 >( x ).get() == v );
    }

    TEST(nested) {
        typedef bitlevel::BitTuple< T10_10, T10_10, BitField< unsigned, 3 > > X;
        X x;
        ASSERT_EQ( X::bitwidth, 43 );
        ASSERT_EQ( X::offset< 0 >(), 0 );
        ASSERT_EQ( X::offset< 1 >(), 20 );
        ASSERT_EQ( X::offset< 2 >(), 40 );
        auto a = get< 0 >( x );
        auto b = get< 1 >( x );
        get< 0 >( a ).set( 5 );
        get< 1 >( a ).set( 7 );
        get< 0 >( b ).set( 13 );
        get< 1 >( b ).set( 533 );
        get< 2 >( x ).set( 15 ); /* we expect to lose the MSB */
        ASSERT_EQ( get< 0 >( a ), 5u );
        ASSERT_EQ( get< 1 >( a ), 7u );
        ASSERT_EQ( get< 0 >( b ), 13u );
        ASSERT_EQ( get< 1 >( b ), 533u );
        ASSERT_EQ( get< 2 >( x ), 7u );
    }

    TEST(locked) {
        bitlevel::BitTuple<
            BitField< int, 15 >,
            BitLock,
            BitField< int, 16 >
        > bt;

        get< 1 >( bt ).lock();

        ASSERT_EQ( get< 0 >( bt ), 0 );
        ASSERT_EQ( get< 2 >( bt ), 0 );
        ASSERT( get< 1 >( bt ).locked() );
        ASSERT( get< 0 >( bt ).word() );

        get< 0 >( bt ) = 1;
        get< 2 >( bt ) = 1;

        ASSERT_EQ( get< 0 >( bt ), 1 );
        ASSERT_EQ( get< 2 >( bt ), 1 );

        ASSERT_EQ( bitcount( get< 0 >( bt ).word() ), 3 );

        get< 1 >( bt ).unlock();
        ASSERT_EQ( get< 0 >( bt ), 1 );
        ASSERT_EQ( get< 2 >( bt ), 1 );
        ASSERT( !get< 1 >( bt ).locked() );

        ASSERT_EQ( bitcount( get< 0 >( bt ).word() ), 2 );

        get< 0 >( bt ) = 0;
        get< 2 >( bt ) = 0;
        ASSERT( !get< 0 >( bt ).word() );
    }

    TEST(assign) {
        bitlevel::BitTuple<
            BitField< bool, 1 >,
            BitField< int, 6 >,
            BitField< bool, 1 >
        > tuple;

        get< 0 >( tuple ) = true;
        get< 2 >( tuple ) = get< 0 >( tuple );
        ASSERT( get< 2 >( tuple ).get() );
    }

    struct OperatorTester {
        int value;
        int expected;
        OperatorTester &operator++() { ASSERT_UNREACHABLE( "fell through" ); return *this; }
        OperatorTester operator++( int ) { ASSERT_UNREACHABLE( "fell through" ); return *this; }
        OperatorTester &operator--() { ASSERT_UNREACHABLE( "fell through" ); return *this; }
        OperatorTester &operator--( int ) { ASSERT_UNREACHABLE( "fell through" ); return *this; }
        OperatorTester &operator+=( int ) { ASSERT_UNREACHABLE( "fell through" ); return *this; }
        OperatorTester &operator-=( int ) { ASSERT_UNREACHABLE( "fell through" ); return *this; }
        OperatorTester &operator*=( int ) { ASSERT_UNREACHABLE( "fell through" ); return *this; }
        OperatorTester &operator/=( int ) { ASSERT_UNREACHABLE( "fell through" ); return *this; }
        OperatorTester &operator%=( int ) { ASSERT_UNREACHABLE( "fell through" ); return *this; }
        void test() { ASSERT_EQ( value, expected ); }
        void set( int v, int e ) { value = v; expected = e; }
    };
    struct TPrI : OperatorTester {
        TPrI &operator++() { ++value; return *this; }
    };
    struct TPoI : OperatorTester {
        TPoI operator++( int ) { auto r = *this; value++; return r; }
    };
    struct TPrD : OperatorTester {
        TPrD &operator--() { --value; return *this; }
    };
    struct TPoD : OperatorTester {
        TPoD operator--( int ) { auto r = *this; value--; return r; }
    };
    struct TPlO : OperatorTester {
        TPlO &operator+=( int v ) { value += v; return *this; }
    };
    struct TMO : OperatorTester {
        TMO &operator-=( int v ) { value -= v; return *this; }
    };
    struct TPoO : OperatorTester {
        TPoO &operator*=( int v ) { value *= v; return *this; }
    };
    struct TSO : OperatorTester {
        TSO &operator/=( int v ) { value /= v; return *this; }
    };
    struct TPrO : OperatorTester {
        TPrO &operator%=( int v ) { value %= v; return *this; }
    };

    template< int N, typename BT, typename L >
    void checkOperator( BT &bt, int v, int e, L l ) {
        auto t = get< N >( bt ).get();
        t.set( v, e );
        get< N >( bt ) = t;
        l( get< N >( bt ) );
        get< N >( bt ).get().test();
    }

#define CHECK( N, bt, v, e, test ) checkOperator< N >( bt, v, e, []( decltype( get< N >( bt ) ) item ) { test; } )

    TEST(operators) {
        bitlevel::BitTuple<
            BitField< bool, 4 >,
            BitField< TPrI >,// ++v
            BitField< TPoI >,// v++
            BitField< TPrD >,// --v
            BitField< TPoD >,// v--
            BitField< TPlO >,// v+=
            BitField< TMO >,// v-=
            BitField< TPoO >,// v*=
            BitField< TSO >,// v/=
            BitField< TPrO >,// v%=
            BitField< bool, 4 >
        > bt;

        CHECK( 1, bt, 0, 1, ++item );
        CHECK( 2, bt, 0, 1, item++ );
        CHECK( 3, bt, 0, -1, --item );
        CHECK( 4, bt, 0, -1, item-- );
        CHECK( 5, bt, 0, 5, item += 5 );
        CHECK( 6, bt, 0, -5, item -= 5 );
        CHECK( 7, bt, 2, 14, item *= 7 );
        CHECK( 8, bt, 42, 6, item /= 7 );
        CHECK( 9, bt, 42, 9, item %= 11 );
    }
#undef CHECK
};

}
}

#endif
// vim: syntax=cpp tabstop=4 shiftwidth=4 expandtab

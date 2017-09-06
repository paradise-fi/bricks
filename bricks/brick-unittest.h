// -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4 -*-

/*
 * Unit testing support code. To run the tests, #include all the files with
 * unit tests in them and run unittest::run(). To get a listing, use
 * unittest::list(). There are examples of unit tests at the end of this file.
 *
 * Unit test registration is only enabled when compiling with
 * -DBRICK_UNITTEST_REG to avoid unneccessary startup-time overhead in normal
 * binaries. See also bricks_unittest in support.cmake.
 */

/*
 * (c) 2006-2014 Petr Ročkai <me@mornfall.net>
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

#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <cxxabi.h>
#include <vector>
#include <set>
#include <map>
#include <stdexcept>

#include <brick-assert.h>

#ifdef __unix
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifndef BRICK_UNITTEST_H
#define BRICK_UNITTEST_H

namespace brick {
namespace unittest {

struct TestCaseBase {
    std::string name;
    bool expect_failure;
    virtual void run() = 0;
    virtual std::string group() = 0;
    std::string id() {
        return group() + "::" + name;
    }
};

struct TestFailed : std::exception {
    const char *what() const noexcept { return "TestFailed"; }
};

namespace {

std::vector< TestCaseBase * > *testcases = nullptr;

#if (defined( __unix ) || defined( POSIX )) && !defined( __divine__ )

void fork_test( TestCaseBase *tc, int *fds ) {
    pid_t pid = fork();
    if ( pid < 0 ) {
        std::cerr << "W: fork failed" << std::endl;
        tc->run(); // well...
    }
    if ( pid == 0 ) {
        if ( fds ) {
            ::dup2( fds[1], 1 );
            ::close( fds[0] );
            ::close( fds[1] );
        }

        try {
            tc->run(); // if anything goes wrong, this should throw
        } catch ( const std::exception &e ) {
            std::cerr << std::endl << "  # case " << tc->id() << " failed: " << e.what() << std::endl;
            exit( 1 );
        }
        exit( 0 );
    }
    if ( pid > 0 ) {
        int status;
        pid_t finished UNUSED = waitpid( pid, &status, 0 );
        ASSERT_EQ( finished, pid );

        if ( WIFEXITED( status ) &&
             WEXITSTATUS( status ) == 0 )
                return;

        if ( WIFSIGNALED( status ) )
            std::cerr << "  # case " << tc->id() << " caught fatal signal " << WTERMSIG( status ) << std::endl;

        throw TestFailed();
    }
}

#else // windows and other abominations

void fork_test( TestCaseBase *tc, int * ) {
    tc->run();
}

#endif

}

#ifdef BRICK_UNITTEST_REG

namespace {

void list() {
    ASSERT( testcases );
    for ( auto tc : *testcases )
        std::cerr << tc->group() << "::" << tc->name << std::endl;
}

std::string simplify( std::string s, std::string l, bool fill = true ) {
    int cut = 0, stop = 0;
    while ( cut < int( s.length() ) && cut < int( l.length() ) && s[cut] == l[cut] ) {
        ++cut;
        if ( l[cut - 1] == ':' )
            stop = cut;
        if ( l[cut] == '<' )
            break;
    }

    while ( cut < int( s.length() ) && s[ cut ] != '<' )
        ++cut;

    if ( s[cut] == '<' ) {
        s = std::string( s, 0, cut + 1 ) +
            simplify( std::string( s, cut + 1, std::string::npos),
                      std::string( s, 0, cut - 1 ), false );
    }

    return (fill ? std::string( stop, ' ' ) : "") + std::string( s, stop, std::string::npos );
}

int run( std::string only_group= "" , std::string only_case = "" ) {
    ASSERT( testcases );
    std::set< std::string > done;
    std::map< std::string, int > counts;
    std::string last;

    int total = 0, total_bad = 0, group_count = 0;

    for ( auto group : *testcases ) {
        ++ counts[ group->group() ];
    }

    for ( auto group : *testcases ) {
        if ( !only_group.empty() && only_group != group->group() )
            continue;
        if ( done.count( group->group() ) )
            continue;

        if ( only_group.empty() )
            std::cerr << "[" << std::setw( 3 ) << 100 * group_count / counts.size() << "%] ";

        std::cerr << simplify( group->group(), last ) << " " << std::flush;

        group_count ++;

        int all = 0, bad = 0;

        for ( auto tc : *testcases ) {
            if ( !only_case.empty() && only_case != tc->name )
                continue;
            if ( group->group() != tc->group() )
                continue;

            bool ok = false;
            try {
                ++ all; ++ total;
                if ( !only_group.empty() && !only_case.empty() )
                    tc->run();
                else
                    fork_test( tc, nullptr );
                ok = true;
            } catch ( const std::exception &e ) {
                if ( e.what() != std::string( "TestFailed" ) )
                    std::cerr << std::endl << "  # case " << tc->id() << " failed: " << e.what() << std::endl;
                std::cerr << group->group() << " " << std::flush;
                ++ bad;
                ++ total_bad;
            }

            if ( ok )
                std::cerr << "." << std::flush;
        }

        std::cerr << " " << (all - bad) << " ok";
        if ( bad )
            std::cerr << ", " << bad << " failed";
        std::cerr << std::endl;
        done.insert( group->group() );
        last = group->group();
    }

    std::cerr << "# summary: " << (total - total_bad) << " ok";
    if ( total_bad )
        std::cerr << ", " << total_bad << " failed";
    std::cerr << std::endl;
    return total_bad > 0;
}

}

#endif

template< typename T >
std::string _typeid() {
#ifdef NO_RTTI
    return "unnamed";
#else
    int stat;
    char *dem = abi::__cxa_demangle( typeid( T ).name(),
                                nullptr, nullptr, &stat );
    std::string strdem( dem );
    std::free( dem );
    return strdem;
#endif
}

template< typename TestGroup, void (TestGroup::*testcase)() >
struct TestCase : TestCaseBase {
    void run() {
        TestGroup tg;
        bool passed = false;
        try {
            (tg.*testcase)();
            passed = true;
        } catch (...) {
            if ( !expect_failure )
                throw;
        }
        if ( passed && expect_failure )
            throw std::runtime_error("test passed unexpectedly");
    }

    std::string group() {
        return _typeid< TestGroup >();
    }

    TestCase() {
        if ( !testcases )
            testcases = new std::vector< TestCaseBase * >;
        testcases->push_back( this );
    }
};

struct RegistrationDone {};

template< template < typename X, void (X::*)() > class Wrapper,
          typename T, void (T::*tc)(), void (T::*reg)() >
void _register_g( const char *n, bool fail ) __attribute__((constructor));

template< template < typename X, void (X::*)() > class Wrapper,
          typename T, void (T::*tc)(), void (T::*reg)() >
void _register_g( const char *n, bool fail )
{
    static int entries = 0;

    if ( entries == 2 )
        return;

    ++ entries;

    static Wrapper< T, tc > t;

    if ( entries == 1 ) {
        try {
            T dummy;
            (dummy.*reg)();
        } catch ( RegistrationDone ) {
            return;
        }
    }

    if ( entries == 2 ) {
        t.name = n;
        t.expect_failure = fail;
        throw RegistrationDone();
    }
}

#undef TEST
#undef TEST_FAILING

#ifndef BRICK_UNITTEST_REG

template< typename T, void (T::*tc)(), void (T::*reg)() = tc >
void _register( const char * )
{
}

#define TEST(n)         void n()
#define TEST_FAILING(n) void n()

#else

template< typename T, void (T::*tc)(), void (T::*reg)() = tc >
void _register( const char *n, bool fail = false )
{
    return _register_g< TestCase, T, tc, reg >( n, fail );
}


#define TEST_(n, bad)                                                   \
    void __reg_ ## n() {                                                \
        using __T = typename std::remove_reference< decltype(*this) >::type; \
        ::brick::unittest::_register< __T, &__T::n, &__T::__reg_ ## n >( #n, bad ); \
    }                                                                   \
    void n()

#define TEST(n)         TEST_(n, false)
#define TEST_FAILING(n) TEST_(n, true)

#endif

/* The following small self-test test group demonstrates how to write unit
 * tests. The TEST(xxx) approach is preferred, but if you want to avoid using
 * macros at all costs, you can use the _register call as shown in the
 * assert_eq test. */

}
}

namespace brick_test {

using namespace ::brick;

namespace unittest {

using namespace ::brick::unittest;

struct SelfTest
{
    TEST(empty) {}

    TEST_FAILING(expected) {
        ASSERT( false );
    }

    void _assert_eq() {
        _register< SelfTest, &SelfTest::_assert_eq >( "assert_eq" );

        bool die UNUSED = true;
        try {
            ASSERT_EQ( 1, 2 );
        } catch ( _assert::AssertFailed ) {
            die = false;
        }
        ASSERT( !die );
    }
};

}
}


#endif

#ifdef BRICK_DEMO

int main( int argc, const char **argv ) {
    return brick::unittest::run( argc > 1 ? argv[1] : "",
                                 argc > 2 ? argv[2] : "" );
}

#endif

// vim: syntax=cpp tabstop=4 shiftwidth=4 expandtab

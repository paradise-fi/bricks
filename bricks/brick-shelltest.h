// -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4 -*-

/*
 * This brick allows you to build a test runner for shell-based functional
 * tests. It comes with fairly elaborate features (although most are only
 * available on posix systems), geared toward difficult-to-test software.
 *
 * It provides a full-featured "main" function (brick::shelltest::run) that you
 * can use as a drop-in shell test runner.
 *
 * Features include:
 * - interactive and batch-mode execution
 * - collects test results and test logs in a simple text-based format
 * - measures resource use of individual tests
 * - rugged: suited for running in monitored virtual machines
 * - supports test flavouring
 */

/*
 * (c) 2014 Petr Ročkai <me@mornfall.net>
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

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <vector>
#include <map>
#include <deque>
#include <string>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <cassert>
#include <iterator>
#include <algorithm>

#ifdef __unix
#include <dirent.h>
#include <sys/stat.h>
#include <sys/resource.h> /* rusage */
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/klog.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef BRICK_SHELLTEST_H
#define BRICK_SHELLTEST_H

namespace brick {
namespace shelltest {

/* TODO: remove this section in favour of brick-filesystem.h */

inline std::runtime_error syserr( std::string msg, std::string ctx = "" ) {
    return std::runtime_error( std::string( strerror( errno ) ) + " " + msg + " " + ctx );
}

struct dir {
    DIR *d;
    dir( std::string p ) {
        d = opendir( p.c_str() );
        if ( !d )
            throw syserr( "error opening directory", p );
    }
    ~dir() { closedir( d ); }
};

typedef std::vector< std::string > Listing;

inline void fsync_name( std::string n )
{
    int fd = open( n.c_str(), O_WRONLY );
    if ( fd >= 0 ) {
        fsync( fd );
        close( fd );
    }
}

inline Listing listdir( std::string p, bool recurse = false, std::string prefix = "" )
{
    Listing r;

    dir d( p );
    struct dirent entry, *iter = 0;
    int readerr;

    while ( (readerr = readdir_r( d.d, &entry, &iter )) == 0 && iter ) {
        std::string ename( entry.d_name );

        if ( ename == "." || ename == ".." )
            continue;

        if ( recurse ) {
            struct stat64 stat;
            std::string s = p + "/" + ename;
            if ( ::stat64( s.c_str(), &stat ) == -1 )
                continue;
            if ( S_ISDIR(stat.st_mode) ) {
                Listing sl = listdir( s, true, prefix + ename + "/" );
                for ( Listing::iterator i = sl.begin(); i != sl.end(); ++i )
                    r.push_back( prefix + *i );
            } else
                r.push_back( prefix + ename );
        } else
            r.push_back( ename );
    };

    if ( readerr != 0 )
        throw syserr( "error reading directory", p );

    return r;
}

/* END remove this section */

struct Journal {
    enum R {
        STARTED,
        RETRIED,
        UNKNOWN,
        FAILED,
        INTERRUPTED,
        KNOWNFAIL,
        PASSED,
        SKIPPED,
        TIMEOUT,
        WARNED,
    };

    friend std::ostream &operator<<( std::ostream &o, R r ) {
        switch ( r ) {
            case STARTED: return o << "started";
            case RETRIED: return o << "retried";
            case FAILED: return o << "failed";
            case INTERRUPTED: return o << "interrupted";
            case PASSED: return o << "passed";
            case SKIPPED: return o << "skipped";
            case TIMEOUT: return o << "timeout";
            case WARNED: return o << "warnings";
            default: return o << "unknown";
        }
    }

    friend std::istream &operator>>( std::istream &i, R &r ) {
        std::string x;
        i >> x;

        r = UNKNOWN;
        if ( x == "started" ) r = STARTED;
        if ( x == "retried" ) r = RETRIED;
        if ( x == "failed" ) r = FAILED;
        if ( x == "interrupted" ) r = INTERRUPTED;
        if ( x == "passed" ) r = PASSED;
        if ( x == "skipped" ) r = SKIPPED;
        if ( x == "timeout" ) r = TIMEOUT;
        if ( x == "warnings" ) r = WARNED;
        return i;
    }

    template< typename S, typename T >
    friend std::istream &operator>>( std::istream &i, std::pair< S, T > &r ) {
        return i >> r.first >> r.second;
    }

    typedef std::map< std::string, R > Status;
    Status status, written;

    std::string location, list;
    int timeouts;

    void append( std::string path ) {
        std::ofstream of( path.c_str(), std::fstream::app );
        Status::iterator writ;
        for ( Status::iterator i = status.begin(); i != status.end(); ++i ) {
            writ = written.find( i->first );
            if ( writ == written.end() || writ->second != i->second )
                of << i->first << " " << i->second << std::endl;
        }
        written = status;
        of.close();
    }

    void write( std::string path ) {
        std::ofstream of( path.c_str() );
        for ( Status::iterator i = status.begin(); i != status.end(); ++i )
            of << i->first << " " << i->second << std::endl;
        of.close();
    }

    void sync() {
        append( location );
        fsync_name( location );
        write ( list );
        fsync_name( list );
    }

    void started( std::string n ) {
        if ( status.count( n ) && status[ n ] == STARTED )
            status[ n ] = RETRIED;
        else
            status[ n ] = STARTED;
        sync();
    }

    void done( std::string n, R r ) {
        status[ n ] = r;
        if ( r == TIMEOUT )
            ++ timeouts;
        else
            timeouts = 0;
        sync();
    }

    bool done( std::string n ) {
        if ( !status.count( n ) )
            return false;
        return status[ n ] != STARTED && status[ n ] != INTERRUPTED;
    }

    int count( R r ) {
        int c = 0;
        for ( Status::iterator i = status.begin(); i != status.end(); ++i )
            if ( i->second == r )
                ++ c;
        return c;
    }

    void banner() {
        std::cout << std::endl << "### " << status.size() << " tests: "
                  << count( PASSED ) << " passed" << std::endl;
    }

    void details() {
        for ( Status::iterator i = status.begin(); i != status.end(); ++i )
            if ( i->second != PASSED )
                std::cout << i->second << ": " << i->first << std::endl;
    }

    void read( std::string n ) {
        std::ifstream ifs( n.c_str() );
        typedef std::istream_iterator< std::pair< std::string, R > > It;
        for ( It i( ifs ); i != It(); ++i )
            status[ i->first ] = i->second;
    }

    void read() { read( location ); }

    Journal( std::string dir )
        : location( dir + "/journal" ),
          list( dir + "/list" ),
          timeouts( 0 )
    {}
};

struct Sink {
    virtual void outline( bool ) {}
    virtual void push( std::string x ) = 0;
    virtual void sync() {}
    virtual ~Sink() {}
};

struct BufSink : Sink {
    std::vector< char > data;
    virtual void push( std::string x ) {
        std::copy( x.begin(), x.end(), std::back_inserter( data ) );
    }

    void dump( std::ostream &o ) {
        std::vector< char >::iterator b = data.begin(), e = data.begin();
        o << std::endl;
        while ( e != data.end() ) {
            e = std::find( b, data.end(), '\n' );
            o << "| " << std::string( b, e ) << std::endl;
            b = (e == data.end() ? e : e + 1);
        }
    }
};

struct FdSink : Sink {
    int fd;

    typedef std::deque< char > Stream;
    typedef std::map< std::string, std::string > Subst;

    Stream stream;
    Subst subst;
    bool killed;

    virtual void outline( bool force )
    {
        Stream::iterator nl = std::find( stream.begin(), stream.end(), '\n' );
        if ( nl == stream.end() ) {
            if ( !force )
                return;
        } else
            force = false;

        assert( nl != stream.end() || force );

        std::string line( stream.begin(), nl );
        stream.erase( stream.begin(), force ? nl : nl + 1 );

        if ( std::string( line, 0, 9 ) == "@TESTDIR=" )
            subst[ "@TESTDIR@" ] = std::string( line, 9, std::string::npos );
        else if ( std::string( line, 0, 8 ) == "@PREFIX=" )
            subst[ "@PREFIX@" ] = std::string( line, 8, std::string::npos );
        else {
            int off;
            for ( Subst::iterator s = subst.begin(); s != subst.end(); ++s )
                while ( (off = line.find( s->first )) != std::string::npos )
                    line.replace( off, s->first.length(), s->second );
            write( fd, line.c_str(), line.length() );
            if ( !force )
                write( fd, "\n", 1 );
        }
    }

    virtual void sync() {
        if ( killed )
            return;
        while ( !stream.empty() )
            outline( true );
    }

    virtual void push( std::string x ) {
        if ( !killed )
            std::copy( x.begin(), x.end(), std::back_inserter( stream ) );
    }

    FdSink( int _fd ) : fd( _fd ), killed( false ) {}
};

struct FileSink : FdSink {
    std::string file;
    FileSink( std::string n ) : FdSink( -1 ), file( n ) {}

    void sync() {
        if ( fd < 0 && !killed ) {
            fd = open( file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644 );
            if ( fd < 0 )
                killed = true;
        }
        FdSink::sync();
    }
    ~FileSink() {
        if ( fd >= 0 ) {
            fsync( fd );
            close( fd );
        }
    }
};

#define BRICK_SYSLOG_ACTION_READ_CLEAR     4
#define BRICK_SYSLOG_ACTION_CLEAR          5

struct KMsg {
    int fd;

    bool dev_kmsg() {
        return fd >= 0;
    }

    void reset() {
#ifdef __unix
        int sz;

        if ( dev_kmsg() ) {
            if ( (fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK)) < 0 ) {
                if (errno != ENOENT) /* Older kernels (<3.5) do not support /dev/kmsg */
                    perror("opening /dev/kmsg");
            } else if (lseek(fd, 0L, SEEK_END) == (off_t) -1)
                perror("lseek /dev/kmsg");
        } else
            klogctl( BRICK_SYSLOG_ACTION_CLEAR, 0, 0 );
#endif
    }

    void read( Sink *s ) {
#ifdef __unix
        int sz;

        char buf[ 128 * 1024 ];

        if ( dev_kmsg() ) {
            while ( (sz = ::read(fd, buf, sizeof(buf) - 1)) > 0 )
                s->push( std::string( buf, sz ) );
            if ( sz < 0 ) {
                fd = -1;
                read( s );
            }
        } else {
            while ( (sz = klogctl( BRICK_SYSLOG_ACTION_READ_CLEAR, buf, sizeof(buf) - 1 )) > 0 )
                s->push( std::string( buf, sz ) );
        }
#endif
    }

    KMsg() : fd( -1 ) {}
};

struct Observer : Sink {
    Observer() {}
    void push( std::string ) {}
};

struct IO : Sink {
    typedef std::vector< Sink* > Sinks;
    mutable Sinks sinks;
    Observer *_observer;

    KMsg kmsg;
    int fd;

    virtual void push( std::string x ) {
        for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
            (*i)->push( x );
    }

    void sync() {
        ssize_t sz;
        char buf[ 128 * 1024 ];

        while ( (sz = read(fd, buf, sizeof(buf) - 1)) > 0 )
            push( std::string( buf, sz ) );

        if ( sz < 0 && errno != EAGAIN )
            throw syserr( "reading pipe" );

        kmsg.read( this );

        for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
            (*i)->sync();
    }

    void close() { ::close( fd ); }
    Observer &observer() { return *_observer; }

    IO() : fd( -1 ) {
        sinks.push_back( _observer = new Observer );
    }

    IO( const IO &io ) {
        fd = io.fd;
        sinks = io.sinks;
        io.sinks.clear();
    }

    IO &operator= ( const IO &io ) {
        fd = io.fd;
        sinks = io.sinks;
        io.sinks.clear();
        return *this;
    }

    void clear() {
        for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
            delete *i;
        sinks.clear();
    }

    ~IO() { clear(); }

};

namespace {
pid_t kill_pid = 0;
bool fatal_signal = false;
bool interrupt = false;
}

struct Options {
    bool verbose, batch, interactive, cont, fatal_timeouts;
    std::string testdir, outdir;
    std::vector< std::string > flavours, filter;
    Options() : verbose( false ), batch( false ), interactive( false ),
                cont( false ), fatal_timeouts( false ) {}
};

struct TestProcess
{
    std::string filename;
    bool interactive;
    int fd;

    void exec() {
        assert( fd >= 0 );
        if ( !interactive ) {
            close( STDIN_FILENO );
            dup2( fd, STDOUT_FILENO );
            dup2( fd, STDERR_FILENO );
            close( fd );
        }

        setpgid( 0, 0 );

        execlp( "bash", "bash", "-noprofile", "-norc", filename.c_str(), NULL );
        perror( "execlp" );
        _exit( 202 );
    }

    TestProcess( std::string file )
        : filename( file ), interactive( false ), fd( -1 )
    {}
};

struct TestCase {
    TestProcess child;
    std::string name, flavour;
    IO io;
    BufSink *iobuf;

    struct rusage usage;
    int status;
    bool timeout;
    pid_t pid;

    time_t start, end, silent_start, last_update, last_heartbeat;
    Options options;

    Journal *journal;

    std::string pretty() {
        if ( options.batch )
            return flavour + ": " + name;
        return "[" + flavour + "] " + name;
    }

    std::string id() {
        return flavour + ":" + name;
    }

    void pipe() {
        int fds[2];

        if (socketpair( PF_UNIX, SOCK_STREAM, 0, fds )) {
            perror("socketpair");
            exit(201);
        }

        if (fcntl( fds[0], F_SETFL, O_NONBLOCK ) == -1) {
            perror("fcntl on socket");
            exit(202);
        }

        io.fd = fds[0];
        child.fd = fds[1];
        child.interactive = options.interactive;
    }

    bool monitor() {
        end = time( 0 );

        /* heartbeat */
        if ( end - last_heartbeat >= 20 ) {
            std::string stampfile( options.outdir + "/timestamp" );
            std::ofstream stamp( stampfile.c_str() );
            stamp << end;
            stamp.close();
            fsync_name( stampfile );
        }

        if ( wait4(pid, &status, WNOHANG, &usage) != 0 ) {
            io.sync();
            return false;
        }

        /* kill off tests after a minute of silence */
        if ( end - silent_start > 60 ) {
            kill( pid, SIGINT );
            sleep( 5 ); /* wait a bit for a reaction */
            if ( waitpid( pid, &status, WNOHANG ) == 0 ) {
                system( "echo t > /proc/sysrq-trigger" );
                kill( -pid, SIGKILL );
                waitpid( pid, &status, 0 );
            }
            timeout = true;
            io.sync();
            return false;
        }

        struct timeval wait;
        fd_set set;

        FD_ZERO( &set );
        FD_SET( io.fd, &set );
        wait.tv_sec = 0;
        wait.tv_usec = 500000; /* timeout 0.5s */

        if ( !options.verbose && !options.interactive && !options.batch ) {
            if ( end - last_update >= 1 ) {
                progress( Update ) << tag( "running" ) << pretty() << " "
                                   << end - start << std::flush;
                last_update = end;
            }
        }

        if ( select( io.fd + 1, &set, NULL, NULL, &wait ) > 0 )
            silent_start = end; /* something happened */

        io.sync();

        return true;
    }

    std::string timefmt( time_t t ) {
        std::stringstream ss;
        ss << t / 60 << ":" << std::setw( 2 ) << std::setfill( '0' ) << t % 60;
        return ss.str();
    }

    std::string rusage()
    {
        std::stringstream ss;
        time_t wall = end - start, user = usage.ru_utime.tv_sec,
             system = usage.ru_stime.tv_sec;
        size_t rss = usage.ru_maxrss / 1024,
               inb = usage.ru_inblock / 100,
              outb = usage.ru_oublock / 100;

        size_t inb_10 = inb % 10, outb_10 = outb % 10;
        inb /= 10; outb /= 10;

        ss << timefmt( wall ) << " wall " << timefmt( user ) << " user "
           << timefmt( system ) << " sys   " << std::setw( 3 ) << rss << "M RSS | "
           << "IOPS: " << std::setw( 5 ) << inb << "." << inb_10 << "K in "
           << std::setw( 5 ) << outb << "." << outb_10 << "K out";
        return ss.str();
    }

    std::string tag( std::string n ) {
        if ( options.batch )
            return "## ";
        int pad = (12 - n.length());
        return "### " + std::string( pad, ' ' ) + n + ": ";
    }

    std::string tag( Journal::R r ) {
        std::stringstream s;
        s << r;
        return tag( s.str() );
    }

    enum P { First, Update, Last };

    std::ostream &progress( P p = Last )
    {
        static struct : std::streambuf {} buf;
        static std::ostream null(&buf);

        if ( options.batch && p == First )
            return std::cout;

        if ( isatty( STDOUT_FILENO ) && !options.batch ) {
            if ( p != First )
                return std::cout << "\r";
            return std::cout;
        }

        if ( p == Last )
            return std::cout;

        return null;
    }

    void parent()
    {
        ::close( child.fd );
        setupIO();

        journal->started( id() );
        silent_start = start = time( 0 );

        progress( First ) << tag( "running" ) << pretty() << std::flush;
        if ( options.verbose || options.interactive )
            progress() << std::endl;

        while ( monitor() );

        Journal::R r = Journal::UNKNOWN;

        if ( timeout ) {
            r = Journal::TIMEOUT;
        } else if ( WIFEXITED( status ) ) {
            if ( WEXITSTATUS( status ) == 0 )
                r = Journal::PASSED;
            else if ( WEXITSTATUS( status ) == 200 )
                r = Journal::SKIPPED;
            else
                r = Journal::FAILED;
        } else if ( interrupt && WIFSIGNALED( status ) && WTERMSIG( status ) == SIGINT )
            r = Journal::INTERRUPTED;
        else
            r = Journal::FAILED;

        io.close();

        if ( iobuf && ( r == Journal::FAILED || r == Journal::TIMEOUT ) )
            iobuf->dump( std::cout );

        journal->done( id(), r );

        if ( options.batch ) {
            int spaces = std::max( 64 - int(pretty().length()), 0 );
            progress( Last ) << " " << std::string( spaces, '.' ) << " " << r << std::endl;
            if ( r == Journal::PASSED )
                progress( First ) << "   " << rusage() << std::endl;
        } else
            progress( Last ) << tag( r ) << pretty() << std::endl;
        io.clear();
    }

    void run() {
        pipe();
        pid = kill_pid = fork();
        if (pid < 0) {
            perror("Fork failed.");
            exit(201);
        } else if (pid == 0) {
            io.close();
            chdir( options.testdir.c_str() );
            setenv("LVM_TEST_FLAVOUR", flavour.c_str(), 1);
            child.exec();
        } else {
            parent();
        }
    }

    void setupIO() {
        iobuf = 0;
        if ( options.verbose )
            io.sinks.push_back( new FdSink( 1 ) );
        else if ( !options.batch )
            io.sinks.push_back( iobuf = new BufSink() );

        std::string n = id();
        std::replace( n.begin(), n.end(), '/', '_' );
        std::string fn = options.outdir + "/" + n + ".txt";
        io.sinks.push_back( new FileSink( fn ) );
    }

    TestCase( Journal &j, Options opt, std::string path, std::string name, std::string flavour )
        : child( path ), name( name ), flavour( flavour ), timeout( false ),
          last_update( 0 ), last_heartbeat( 0 ), options( opt ), journal( &j )
    {
    }
};

struct Main {
    bool die;
    time_t start;

    typedef std::vector< TestCase > Cases;
    typedef std::vector< std::string > Flavours;

    Journal journal;
    Options options;
    Cases cases;

    void setup() {
        Listing l = listdir( options.testdir, true );
        std::sort( l.begin(), l.end() );

        for ( Flavours::iterator flav = options.flavours.begin();
              flav != options.flavours.end(); ++flav ) {

            for ( Listing::iterator i = l.begin(); i != l.end(); ++i ) {
                if ( i->substr( i->length() - 3, i->length() ) != ".sh" )
                    continue;
                if ( i->substr( 0, 4 ) == "lib/" )
                    continue;
                bool filter = !options.filter.empty();

                for ( std::vector< std::string >::iterator filt = options.filter.begin();
                      filt != options.filter.end(); ++filt ) {
                    if ( i->find( *filt ) != std::string::npos )
                        filter = false;
                }
                if ( filter )
                    continue;
                cases.push_back( TestCase( journal, options, options.testdir + *i, *i, *flav ) );
                cases.back().options = options;
            }
        }

        if ( options.cont )
            journal.read();
        else
            ::unlink( journal.location.c_str() );
    }

    int run() {
        setup();
        start = time( 0 );
        std::cerr << "running " << cases.size() << " tests" << std::endl;

        for ( Cases::iterator i = cases.begin(); i != cases.end(); ++i ) {

            if ( options.cont && journal.done( i->id() ) )
                continue;

            i->run();

            if ( options.fatal_timeouts && journal.timeouts >= 2 ) {
                journal.started( i->id() ); // retry the test on --continue
                std::cerr << "E: Hit 2 timeouts in a row with --fatal-timeouts" << std::endl;
                std::cerr << "Suspending (please restart the VM)." << std::endl;
                sleep( 3600 );
                die = 1;
            }

            if ( time(0) - start > 3 * 3600 ) {
                std::cerr << "3 hours passed, giving up..." << std::endl;
                die = 1;
            }

            if ( die || fatal_signal )
                break;
        }

        journal.banner();
        if ( die || fatal_signal )
            return 1;

        return 0;
    }

    Main( Options o ) : die( false ), journal( o.outdir ), options( o ) {}
};

namespace {

void handler( int sig ) {
    signal( sig, SIG_DFL ); /* die right away next time */
    if ( kill_pid > 0 )
        kill( -kill_pid, sig );
    fatal_signal = true;
    if ( sig == SIGINT )
        interrupt = true;
}

void setup_handlers() {
    /* set up signal handlers */
    for ( int i = 0; i <= 32; ++i )
        switch (i) {
            case SIGCHLD: case SIGWINCH: case SIGURG:
            case SIGKILL: case SIGSTOP: break;
            default: signal(i, handler);
        }
}

int64_t get_time_us(void)
{
    struct timeval tv;
    (void) gettimeofday(&tv, 0);
    return (int64_t) tv.tv_sec * 1000000 + (int64_t) tv.tv_usec;
}

}

/* TODO remove in favour of brick-commandline.h */
struct Args {
    typedef std::vector< std::string > V;
    V args;

    Args( int argc, const char **argv ) {
        for ( int i = 1; i < argc; ++ i )
            args.push_back( argv[ i ] );
    }

    bool has( std::string fl ) {
        return std::find( args.begin(), args.end(), fl ) != args.end();
    }

    std::string opt( std::string fl ) {
        V::iterator i = std::find( args.begin(), args.end(), fl );
        if ( i == args.end() || i + 1 == args.end() )
            return "";
        return *(i + 1);
    }
};

namespace {

bool hasenv( const char *name ) {
    const char *v = getenv( name );
    if ( !v )
        return false;
    if ( strlen( v ) == 0 || !strcmp( v, "0" ) )
        return false;
    return true;
}

template< typename C >
void split( std::string s, C &c ) {
    std::stringstream ss( s );
    std::string item;
    while ( std::getline( ss, item, ',' ) )
        c.push_back( item );
}

}

int run( int argc, const char **argv )
{
    Args args( argc, argv );
    Options opt;

    if ( args.has( "--continue" ) )
        opt.cont = true;

    if ( args.has( "--only" ) )
        split( args.opt( "--only" ), opt.filter );

    if ( args.has( "--fatal-timeouts" ) )
        opt.fatal_timeouts = true;

    if ( args.has( "--batch" ) || hasenv( "BATCH" ) ) {
        opt.verbose = false;
        opt.batch = true;
    }

    if ( args.has( "--verbose" ) || hasenv( "VERBOSE" ) ) {
        opt.batch = false;
        opt.verbose = true;
    }

    if ( args.has( "--interactive" ) || hasenv( "INTERACTIVE" ) ) {
        opt.verbose = false;
        opt.batch = false;
        opt.interactive = true;
    }

    if ( args.has( "--flavours" ) )
        split( args.opt( "--flavours" ), opt.flavours );
    else
        opt.flavours.push_back( "vanilla" );

    opt.outdir = args.opt( "--outdir" );
    opt.testdir = args.opt( "--testdir" );

    if ( opt.testdir.empty() )
        opt.testdir = "/usr/share/lvm2-testsuite";

    opt.testdir += "/";

    setup_handlers();

    Main main( opt );
    return main.run();
}

}
}

#endif

#ifdef BRICK_DEMO

int main( int argc, const char **argv ) {
    brick::shelltest::run( argc, argv );
}

#endif

// vim: tabstop=4 shiftwidth=4 expandtab

// -*- C++ -*- (c) 2014 Vladimír Štill

#ifdef BRICKS_HAVE_LLVM

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif
#include <llvm/Linker.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <brick-assert.h>

#include <vector>
#include <string>
#include <initializer_list>

#ifndef BRICKS_LLVM_LINK_STRIP_H
#define BRICKS_LLVM_LINK_STRIP_H

namespace brick {
namespace llvm {

enum class Prune { UnusedModules, AllUnused };

inline bool isPrefixOf( std::string a, std::string b ) {
    if ( a.size() > b.size() )
        return false;
    auto r = std::mismatch( a.begin(), a.end(), b.begin() );
    return r.first == a.end();
}

struct ModuleMap {

    struct ModuleRef {
        ModuleRef( const ModuleMap *mm, int id ) : _id( id ), _map( mm ) {
            ASSERT( mm );
            ASSERT_LEQ( 0, id );
            ASSERT_LEQ( id, int( _map->_idToM.size() ) - 1 );
        }

        // we order modules such that modules added later will be first
        // in std::set< ModuleRef >
        bool operator<( const ModuleRef &o ) const {
            ASSERT_EQ( _map, o._map );
            return _id > o._id;
        }

        int id() const { return _id; }
        std::string name() const { return _map->_idToM[ _id ]; }

      private:
        int _id;
        const ModuleMap *_map;
    };

    std::pair< ModuleRef, bool > insert( std::string mod ) {
        auto it = _mToId.find( mod );
        if ( it == _mToId.end() ) {
            int id = _idToM.size();
            _mToId[ mod ] = id;
            _idToM.emplace_back( std::move( mod ) );
            return std::make_pair( ModuleRef( this, id ), true );
        }
        return std::make_pair( ModuleRef( this, it->second ), false );
    }

    ModuleRef operator[]( std::string mod ) const {
        auto it = _mToId.find( mod );
        ASSERT( it != _mToId.end() );
        return ModuleRef( this, it->second );
    }

    ModuleRef operator[]( int id ) const {
        return ModuleRef( this, id );
    }

  private:
    std::map< std::string, int > _mToId;
    std::vector< std::string > _idToM;
};

using ModuleRef = ModuleMap::ModuleRef;

struct Linker {
    using Module = ::llvm::Module;
    static constexpr auto modulePrefix = "brick-llvm.module.";
    static constexpr auto moduleRoot = "brick-llvm.module_root";
    static constexpr auto ctorPrefix = "brick-llvm.ctor.";
    static constexpr auto global_ctors = "llvm.global_ctors";

    Linker() { }

    // load parlially linked module and take ownership of it
    void load( Module *prelinked ) {
        ASSERT( prelinked != nullptr );
        _link.reset( new ::llvm::Linker( prelinked ) );
        _root.reset( prelinked );

        auto modlist = _root->getOrInsertNamedMetadata( moduleRoot );
        for ( int i = 0, count = modlist->getNumOperands(); i < count; ++i ) {
            std::string mod = ::llvm::cast< ::llvm::MDString >(
                    modlist->getOperand( i )->getOperand( 0 ) )->getString().data();
            _modules.insert( mod );
        }
    }

    // link bitcode module fresh from source (not yet linked)
    void link( ::llvm::Module *src ) {
        ASSERT( src != nullptr );
        if ( !_link ) {
            ASSERT( !_root );
            _root.reset( src );
            _link.reset( new ::llvm::Linker( _annotate( _root.get() ) ) );
        } else {
            _owned.emplace_back( src );
            std::string err;
            auto r UNUSED = _link->linkInModule( _annotate( src ), ::llvm::Linker::DestroySource, &err );
            ASSERT( !r );
        }
    }

    template< typename Roots = std::initializer_list< std::string > >
    Module *prune( Roots roots, Prune prune = Prune::UnusedModules ) {
        using ::llvm::ConstantArray;
        using ::llvm::GlobalVariable;
        using ::llvm::GlobalValue;
        using ::llvm::ArrayType;
        using ::llvm::Constant;
        using ::llvm::ArrayRef;
        using ::llvm::cast;


        _Prune pruner( this, roots );
        auto m = pruner.pruneModules();

        // build llvm.global_ctors from used module ctors
        std::vector< ConstantArray * > ctors;
        for ( auto &glo : m->getGlobalList() )
            if ( isPrefixOf( ctorPrefix, glo.getName().data() ) )
                ctors.emplace_back( cast< ConstantArray >( glo.getInitializer() ) );

        if ( !ctors.empty() ) {
            auto valType = ctors.front()->getType()->getElementType();
            int cnt = 0;
            for ( auto v : ctors )
                cnt += v->getNumOperands();

            std::vector< ::llvm::Constant * > ops;
            for ( auto &c : ctors )
                for ( auto i = c->op_begin(); i != c->op_end(); ++i ) {
                    ::llvm::Use *use = &*i;
                    ops.emplace_back( cast< Constant >( use ) );
                }

            auto ctorType = ArrayType::get( valType, cnt );
            auto values = ConstantArray::get( ctorType, ArrayRef< Constant * >( ops ) );
            ASSERT( ctorType );
            ASSERT( values );
            new GlobalVariable( *m, ctorType, false,
                    GlobalValue::ExternalLinkage,
                    values, global_ctors );
        }

        return prune == Prune::UnusedModules
            ? m : pruner.pruneUnused();
    }

    Module *get() { return _root.get(); }
    const Module *get() const { return _root.get(); }


  private:
    std::unique_ptr< ::llvm::Linker > _link;
    ModuleMap _modules;
    std::unique_ptr< Module > _root;
    std::vector< std::unique_ptr< Module > > _owned;
    using ValsRef = ::llvm::ArrayRef< ::llvm::Value * >;

    Module *_annotate( Module *m ) {
        auto id = _modules.insert( m->getModuleIdentifier() ).first.name();
        auto mdroot = m->getOrInsertNamedMetadata( modulePrefix + id );

        for ( auto &fn : *m )
            if ( !fn.isDeclaration() )
                mdroot->addOperand( _mdlink( m, fn ) );
        for ( auto &glo : m->getGlobalList() )
            if ( !glo.isDeclaration() )
                mdroot->addOperand( _mdlink( m, glo ) );
        for ( auto &ali : m->getAliasList() )
            if ( !ali.isDeclaration() )
                mdroot->addOperand( _mdlink( m, ali ) );

        if ( auto ctors = m->getGlobalVariable( global_ctors ) )
            ctors->setName( ctorPrefix + id );

        auto root = _root ? _root.get() : m;
        auto modlist = root->getOrInsertNamedMetadata( moduleRoot );
        modlist->addOperand( _modmd( root, id ) );
        return m;
    }

    ::llvm::MDNode *_mdlink( Module *m, ::llvm::Value &val ) {
        return ::llvm::MDNode::get( m->getContext(), ValsRef( { &val } ) );
    }

    ::llvm::MDNode *_modmd( Module *m, std::string modname ) {
        return ::llvm::MDNode::get( m->getContext(), ValsRef(
                    { ::llvm::MDString::get( m->getContext(), modname ) } ) );
    }

    struct _Prune {
        Linker *_link;
        std::vector< ::llvm::Value * > _stack;
        std::set< ::llvm::Value * > _seen;
        std::set< ModuleRef > _seenModules;
        std::map< std::string, std::set< ::llvm::Value * > > _moduleMap;
        std::map< ::llvm::Value *, std::set< ModuleRef > > _symbolModule;
        std::vector< std::string > _roots;
        long _functions = 0, _globals = 0;

        template< typename Roots >
        _Prune( Linker *ln, Roots roots ) : _link( ln ), _roots( roots ) { }

        void buildModuleMaps() {
            _moduleMap.clear();
            _symbolModule.clear();

            auto m = _link->get();

            for ( auto &meta : m->getNamedMDList() ) {
                if ( isPrefixOf( modulePrefix, meta.getName().data() ) ) {
                    auto mname = std::string( meta.getName().data() ).substr( std::string( modulePrefix ).size() );
                    auto &mset = _moduleMap[ mname ];
                    for ( int i = 0, count = meta.getNumOperands(); i < count; ++i ) {
                        auto sym = meta.getOperand( i )->getOperand( 0 );
                        mset.insert( sym );
                        _symbolModule[ sym ].insert( _link->_modules[ mname ] );
                    }
                }
            }
        }

        bool pushSimple( ::llvm::Value *val ) {
            if ( _seen.insert( val ).second ) {
                _stack.push_back( val );
                return true;
            }
            return false;
        }

        void pushWithModule( ::llvm::Value *val ) {
            if ( pushSimple( val ) ) {
                // There might be symbols which are not in symbol <-> module table,
                // such symbols are anything else than functions, globals and alisases
                // (for example instructions).
                // Furthrermore, for each symbol we add just one module: the last
                // linked module which contains it (set of modules inside _symbolModule
                // is sorted by reverse order of linking).
                // Also we must include whole module when adding it, so we must
                // look into all functions of the module
                auto mods = _symbolModule.find( val );
                if ( mods != _symbolModule.end() ) {
                    if ( std::any_of( mods->second.begin(), mods->second.end(),
                                [&]( ModuleRef r ) -> bool { return _seenModules.count( r ); } ) )
                        return;
                    auto &mod = *mods->second.begin();
                    if ( _seenModules.insert( mod ).second ) {
                        for ( auto &sym : _moduleMap[ mod.name() ] )
                            pushSimple( sym );
                    }
                }
            }
        }

        Module *pruneModules() {
            return prune( [&]( ::llvm::Value *v ) { pushWithModule( v ); } );
        }

        Module *pruneUnused() {
            return prune( [&]( ::llvm::Value *v ) { pushSimple( v ); } );
        }

        template< typename Push >
        void init( Push push ) {
            _stack.clear();
            _seen.clear();
            _seenModules.clear();

            for ( auto sym : _roots )
                if ( auto root = _link->get()->getFunction( sym ) )
                    push( root );
            for ( auto sym : _roots )
                if ( auto root = _link->get()->getGlobalVariable( sym, true ) )
                    push( root );
            if ( auto ctors = _link->get()->getGlobalVariable( global_ctors ) )
                push( ctors );
        }

        template< typename Push >
        Module *prune( Push push ) {
            buildModuleMaps();
            init( push );

            auto handleVal = [&]( ::llvm::Value *v ) { _handleVal( push, v ); };

            while ( _stack.size() ) {
                auto val = _stack.back();
                _stack.pop_back();
                ASSERT( val != nullptr );

                handleVal( val );
            }

            auto m = _link->get();

            std::vector< ::llvm::GlobalValue* > toDrop;

            for ( auto &fn : *m )
                if ( _seen.count( &fn ) == 0 )
                    toDrop.push_back( &fn );

            for ( auto &glo : m->getGlobalList() )
                if ( _seen.count( &glo ) == 0 )
                    toDrop.push_back( &glo );

            for ( auto &ali : m->getAliasList() )
                if ( _seen.count( &ali ) == 0 )
                    toDrop.push_back( &ali );

            /* we must first delete bodies of functions (and globals),
             * then drop referrences to them (replace with undef)
             * finally we can erase symbols
             * Those must be separate runs, otherwise we could trip SEGV
             * by accessing already freed symbol by some use
             */
            for ( auto glo : toDrop )
                glo->dropAllReferences();

            for ( auto glo : toDrop )
                glo->replaceAllUsesWith( ::llvm::UndefValue::get( glo->getType() ) );

            for ( auto glo : toDrop ) {
                glo->eraseFromParent();
            }

            return m;
        }

        template< typename Push >
        void _handleVal( Push push, ::llvm::Value *val ) {
            ASSERT( val != nullptr );

            if ( auto fn = ::llvm::dyn_cast< ::llvm::Function >( val ) ) {
                ++_functions;

                for ( const auto &bb : *fn )
                    for ( const auto &ins : bb )
                        for ( auto op = ins.op_begin(); op != ins.op_end(); ++op )
                            push( op->get() );
            }

            else if ( auto bb = ::llvm::dyn_cast< ::llvm::BasicBlock >( val ) )
                push( bb->getParent() );

            else if ( auto ba = ::llvm::dyn_cast< ::llvm::BlockAddress >( val ) )
                push( ba->getFunction() );

            else if ( auto ali = ::llvm::dyn_cast< ::llvm::GlobalAlias >( val ) )
                push( ali->getAliasee() );

            else if ( auto global = ::llvm::dyn_cast< ::llvm::GlobalValue >( val ) ) {
                ++_globals;

                if ( auto gvar = ::llvm::dyn_cast< ::llvm::GlobalVariable >( global ) )
                    if ( gvar->hasInitializer() )
                        push( gvar->getInitializer() );
            }

            // bitcast function pointers
            else if ( auto ce = ::llvm::dyn_cast< ::llvm::ConstantExpr >( val ) )
                push( ce->getAsInstruction() );

            // catchall: instructions, constants, ...
            else if ( auto ins = ::llvm::dyn_cast< ::llvm::User >( val ) )
                for ( auto op = ins->op_begin(); op != ins->op_end(); ++op )
                    push( op->get() );
        }
    };
};

inline void writeModule( ::llvm::Module *m, std::string out ) {
    std::string serr;
    ::llvm::raw_fd_ostream fs( out.c_str(), serr );
    WriteBitcodeToFile( m, fs );
}

}
}

#endif // BRICKS_LLVM_LINK_STRIP_H

#endif

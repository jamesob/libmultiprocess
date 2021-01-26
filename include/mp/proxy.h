// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MP_PROXY_H
#define MP_PROXY_H

#include <mp/util.h>

#include <array>
#include <functional>
#include <list>
#include <stddef.h>
#include <tuple>
#include <type_traits>
#include <utility>

namespace mp {
class Connection;
class EventLoop;
//! Mapping from capnp interface type to proxy client implementation (specializations are generated by
//! proxy-codegen.cpp).
template <typename Interface>
struct ProxyClient;
//! Mapping from capnp interface type to proxy server implementation (specializations are generated by
//! proxy-codegen.cpp).
template <typename Interface>
struct ProxyServer;
//! Mapping from capnp method params type to method traits (specializations are generated by proxy-codegen.cpp).
template <typename Params>
struct ProxyMethod;
//! Mapping from capnp struct type to struct traits (specializations are generated by proxy-codegen.cpp).
template <typename Struct>
struct ProxyStruct;
//! Mapping from local c++ type to capnp type and traits (specializations are generated by proxy-codegen.cpp).
template <typename Type>
struct ProxyType;

using CleanupList = std::list<std::function<void()>>;
using CleanupIt = typename CleanupList::iterator;

//! Context data associated with proxy client and server classes.
struct ProxyContext
{
    Connection* connection;

    ProxyContext(Connection* connection) : connection(connection) {}
};

//! Base class for generated ProxyClient classes that implement a C++ interface
//! and forward calls to a capnp interface.
template <typename Interface_, typename Impl_>
class ProxyClientBase : public Impl_
{
public:
    using Interface = Interface_;
    using Impl = Impl_;

    ProxyClientBase(typename Interface::Client client, Connection* connection, bool destroy_connection);
    ~ProxyClientBase() noexcept;

    // Methods called during client construction/destruction that can optionally
    // be defined in capnp interface to trigger the server.
    void construct() {}
    void destroy() {}

    ProxyClient<Interface>& self() { return static_cast<ProxyClient<Interface>&>(*this); }

    typename Interface::Client m_client;
    ProxyContext m_context;
    bool m_destroy_connection;
    CleanupIt m_cleanup; //!< Pointer to self-cleanup callback registered to handle connection object getting destroyed
                         //!< before this client object.
};

//! Customizable (through template specialization) base class used in generated ProxyClient implementations from
//! proxy-codegen.cpp.
template <typename Interface, typename Impl>
class ProxyClientCustom : public ProxyClientBase<Interface, Impl>
{
    using ProxyClientBase<Interface, Impl>::ProxyClientBase;
};

//! Base class for generated ProxyServer classes that implement capnp server
//! methods and forward calls to a wrapped c++ implementation class.
template <typename Interface_, typename Impl_>
struct ProxyServerBase : public virtual Interface_::Server
{
public:
    using Interface = Interface_;
    using Impl = Impl_;

    ProxyServerBase(std::shared_ptr<Impl> impl, Connection& connection);
    virtual ~ProxyServerBase();
    void invokeDestroy();

    /**
     * Implementation pointer that may or may not be owned and deleted when this
     * capnp server goes out of scope. It is owned for servers created to wrap
     * unique_ptr<Impl> method arguments, but unowned for servers created to
     * wrap Impl& method arguments.
     *
     * In the case of Impl& arguments, custom code is required on other side of
     * the connection to delete the capnp client & server objects since native
     * code on that side of the connection will just be taking a plain reference
     * rather than a pointer, so won't be able to do its own cleanup. Right now
     * this is implemented with addCloseHook callbacks to delete clients at
     * appropriate times depending on semantics of the particular method being
     * wrapped. */
    std::shared_ptr<Impl> m_impl;
    ProxyContext m_context;
};

//! Customizable (through template specialization) base class used in generated ProxyServer implementations from
//! proxy-codegen.cpp.
template <typename Interface, typename Impl>
struct ProxyServerCustom : public ProxyServerBase<Interface, Impl>
{
    using ProxyServerBase<Interface, Impl>::ProxyServerBase;
};

//! Function traits class used to get method paramater and result types in generated ProxyClient implementations from
//! proxy-codegen.cpp.
template <class Fn>
struct FunctionTraits;

//! Specialization of above to extract result and params types.
template <class _Class, class _Result, class... _Params>
struct FunctionTraits<_Result (_Class::*const)(_Params...)>
{
    using Params = TypeList<_Params...>;
    using Result = _Result;
    template <size_t N>
    using Param = typename std::tuple_element<N, std::tuple<_Params...>>::type;
    using Fields =
        typename std::conditional<std::is_same<void, Result>::value, Params, TypeList<_Params..., _Result>>::type;
};

//! Traits class for a method specialized by method parameters.
//!
//! Param and Result typedefs can be customized to adjust parameter and return types on client side.
//!
//! Invoke method customized to adjust parameter and return types on server side.
//!
//! Normal method calls go through the ProxyMethodTraits struct specialization
//! below, not this default struct, which is only used if there is no
//! ProxyMethod::impl method pointer, which is only true for construct/destroy
//! methods.
template <typename MethodParams, typename Enable = void>
struct ProxyMethodTraits
{
    using Params = TypeList<>;
    using Result = void;
    using Fields = Params;

    template <typename ServerContext>
    static void invoke(ServerContext&)
    {
    }
};

//! Specialization of above.
template <typename MethodParams>
struct ProxyMethodTraits<MethodParams, Require<decltype(ProxyMethod<MethodParams>::impl)>>
    : public FunctionTraits<decltype(ProxyMethod<MethodParams>::impl)>
{
    template <typename ServerContext, typename... Args>
    static auto invoke(ServerContext& server_context, Args&&... args) -> AUTO_RETURN(
        (server_context.proxy_server.m_impl.get()->*ProxyMethod<MethodParams>::impl)(std::forward<Args>(args)...))
};

//! Customizable (through template specialization) traits class used in generated ProxyClient implementations from
//! proxy-codegen.cpp.
template <typename MethodParams>
struct ProxyClientMethodTraits : public ProxyMethodTraits<MethodParams>
{
};

//! Customizable (through template specialization) traits class used in generated ProxyServer implementations from
//! proxy-codegen.cpp.
template <typename MethodParams>
struct ProxyServerMethodTraits : public ProxyMethodTraits<MethodParams>
{
};

static constexpr int FIELD_IN = 1;
static constexpr int FIELD_OUT = 2;
static constexpr int FIELD_OPTIONAL = 4;
static constexpr int FIELD_REQUESTED = 8;
static constexpr int FIELD_BOXED = 16;

//! Accessor type holding flags that determine how to access a message field.
template <typename Field, int flags>
struct Accessor : public Field
{
    static const bool in = flags & FIELD_IN;
    static const bool out = flags & FIELD_OUT;
    static const bool optional = flags & FIELD_OPTIONAL;
    static const bool requested = flags & FIELD_REQUESTED;
    static const bool boxed = flags & FIELD_BOXED;
};

//! Wrapper around std::function for passing std::function objects between client and servers.
template <typename Fn>
class ProxyCallback;

//! Specialization of above to separate Result and Arg types.
template <typename Result, typename... Args>
class ProxyCallback<std::function<Result(Args...)>>
{
public:
    virtual Result call(Args&&... args) = 0;
};

} // namespace mp

#endif // MP_PROXY_H

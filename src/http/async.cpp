#include <supabase/http.hpp>

#include <exception>
#include <thread>

namespace supabase::http
{

namespace
{
    Result<Response> sendGuarded(Transport& transport, const Request& request)
    {
        try
        {
            return transport.send(request);
        }
        catch (const std::exception& e)
        {
            return makeError(errc::Network, e.what());
        }
        catch (...)
        {
            return makeError(errc::Network, "unknown transport failure");
        }
    }
}

void sendAsync(std::shared_ptr<Transport> transport, Request request, std::function<void(Result<Response>)> done)
{
    if (!transport || !done)
        return;
    try
    {
        std::thread([transport = std::move(transport), request = std::move(request), done = std::move(done)]() {
            done(sendGuarded(*transport, request));
        }).detach();
    }
    catch (...)
    {
        // Thread creation failed (e.g. resource exhaustion): report instead of dropping the callback.
        done(makeError(errc::Network, "failed to start worker thread"));
    }
}

std::future<Result<Response>> sendAsync(std::shared_ptr<Transport> transport, Request request)
{
    auto promise = std::make_shared<std::promise<Result<Response>>>();
    auto future  = promise->get_future();
    if (!transport)
    {
        promise->set_value(makeError(errc::InvalidArgument, "null transport"));
        return future;
    }
    sendAsync(std::move(transport), std::move(request), [promise](Result<Response> result) { promise->set_value(std::move(result)); });
    return future;
}

}

// ============================================================================
//  zenith::coro Redis 等待器 —— co_await 形式的 Redis 命令（C++20 可选层）
//  用法：
//    redis::RedisClient client(&loop); ... connect ...
//    auto reply = zenith::coro::syncWait(zenith::coro::redisCommand(client, {"GET", "k"}));
// ============================================================================
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "zenith/coro/callback_await.hpp"
#include "zenith/core/result.hpp"
#include "zenith/redis/redis_client.hpp"

namespace zenith::coro {

/// co_await redisCommand(client, {"SET", "k", "v"}) → Task<redis::RedisReply>；失败抛异常。
inline Task<redis::RedisReply> redisCommand(redis::RedisClient& client,
                                            std::vector<std::string> args) {
    co_return co_await detail::awaitCallback<redis::RedisReply>(
        [&client, args = std::move(args)](auto cb) mutable {
            client.command(std::move(args), cb);
        });
}

} // namespace zenith::coro

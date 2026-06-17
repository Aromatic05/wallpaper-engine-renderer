#pragma once

#include <string>
#include <utility>

namespace wallpaper
{
enum class ResultCode
{
    Ok = 0,
    InvalidArgument,
    InvalidState,
    NotSupported,
    NotFound,
    InternalError
};

struct Error {
    ResultCode  code { ResultCode::Ok };
    std::string message;
};

template<typename T>
class Result {
public:
    Result(T value)
        : m_ok(true)
        , m_value(std::move(value)) {}

    Result(const Error& error)
        : m_ok(false)
        , m_error(error) {}

    Result(Error&& error)
        : m_ok(false)
        , m_error(std::move(error)) {}

    static Result<T> success(T value) { return Result<T>(std::move(value)); }

    static Result<T> failure(ResultCode code, std::string message) {
        return Result<T>(Error { code, std::move(message) });
    }

    bool ok() const { return m_ok; }
    explicit operator bool() const { return ok(); }

    const T& value() const { return m_value; }
    T&       value() { return m_value; }

    const Error& error() const { return m_error; }

private:
    bool  m_ok { false };
    T     m_value {};
    Error m_error {};
};

template<>
class Result<void> {
public:
    Result() = default;

    Result(const Error& error)
        : m_ok(false)
        , m_error(error) {}

    Result(Error&& error)
        : m_ok(false)
        , m_error(std::move(error)) {}

    static Result<void> success() { return Result<void>(); }

    static Result<void> failure(ResultCode code, std::string message) {
        return Result<void>(Error { code, std::move(message) });
    }

    bool ok() const { return m_ok; }
    explicit operator bool() const { return ok(); }

    const Error& error() const { return m_error; }

private:
    bool  m_ok { true };
    Error m_error {};
};
} // namespace wallpaper

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace bdr::core
{
    enum class ErrorCode
    {
        InvalidArgument,
        InvalidState,
        Configuration,
        InvalidDll,
        ArchitectureMismatch,
        LoadFailure,
        MissingExport,
        FactoryFailure,
        UnsupportedInterface,
        DriverFailure,
        InvalidStream,
        ReceiveTimeout,
        FileFailure,
        LogFailure,
        NotImplemented
    };

    class RelayError : public std::runtime_error
    {
      public:
        RelayError(ErrorCode code, std::string message, std::uint32_t native_code = 0);
        [[nodiscard]] ErrorCode GetCode() const noexcept;
        [[nodiscard]] std::uint32_t GetNativeCode() const noexcept;

      private:
        ErrorCode m_code;
        std::uint32_t m_nativeCode;
    };

    [[nodiscard]] const char* GetErrorCodeName(ErrorCode code) noexcept;
} // namespace bdr::core

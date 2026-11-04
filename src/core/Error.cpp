#include "BonDriverRelay/core/Error.h"

#include <utility>

namespace bdr::core
{
    RelayError::RelayError(ErrorCode code, std::string message, std::uint32_t native_code)
        : std::runtime_error(std::move(message)), m_code(code), m_nativeCode(native_code)
    {}

    ErrorCode RelayError::GetCode() const noexcept
    {
        return m_code;
    }
    std::uint32_t RelayError::GetNativeCode() const noexcept
    {
        return m_nativeCode;
    }

    const char* GetErrorCodeName(ErrorCode code) noexcept
    {
        switch (code) {
        case ErrorCode::InvalidArgument:
            return "InvalidArgument";
        case ErrorCode::InvalidState:
            return "InvalidState";
        case ErrorCode::Configuration:
            return "Configuration";
        case ErrorCode::InvalidDll:
            return "InvalidDll";
        case ErrorCode::ArchitectureMismatch:
            return "ArchitectureMismatch";
        case ErrorCode::LoadFailure:
            return "LoadFailure";
        case ErrorCode::MissingExport:
            return "MissingExport";
        case ErrorCode::FactoryFailure:
            return "FactoryFailure";
        case ErrorCode::UnsupportedInterface:
            return "UnsupportedInterface";
        case ErrorCode::DriverFailure:
            return "DriverFailure";
        case ErrorCode::InvalidStream:
            return "InvalidStream";
        case ErrorCode::ReceiveTimeout:
            return "ReceiveTimeout";
        case ErrorCode::FileFailure:
            return "FileFailure";
        case ErrorCode::LogFailure:
            return "LogFailure";
        case ErrorCode::NotImplemented:
            return "NotImplemented";
        }
        return "Unknown";
    }
} // namespace bdr::core

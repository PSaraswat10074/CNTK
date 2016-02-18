#pragma once
#include <stdexcept>
namespace Microsoft {
namespace MSR {
namespace CNTK {

// Exception wrapper to include native call stack string
template <class E>
class RichException : public E
{
public:
    RichException(std::string& msg, std::string& callstack) : E(msg), mCallStack(callstack.begin(), callstack.end())
    {}

    std::wstring callStack() const
    {
        return mCallStack.c_str();
    }
protected:
    std::wstring mCallStack;
};

template class RichException<std::runtime_error>;
template class RichException<std::logic_error>;
template class RichException<std::invalid_argument>;
}
}
}
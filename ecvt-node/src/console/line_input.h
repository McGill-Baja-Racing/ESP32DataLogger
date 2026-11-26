#pragma once
#include <cstddef>

// Stream framing independent of stdio read boundaries. Never parse a truncated
// line or a line containing NUL/terminal escape sequences as a valid command.
struct ConsoleLineInput {
    enum class Result { Pending, Empty, Ready, Overflow, InvalidByte };
    char text[160] = {};
    std::size_t length = 0;
    bool overflow = false, invalid = false;

    Result push(unsigned char c) {
        if (c == '\r' || c == '\n') {
            text[length] = '\0';
            return overflow ? Result::Overflow : invalid ? Result::InvalidByte :
                   length ? Result::Ready : Result::Empty;
        }
        if (c == '\b' || c == 127) {
            if (!overflow && length) --length;
        } else if (!overflow) {
            if (length + 1 >= sizeof(text)) overflow = true;
            else {
                if ((c < 32 && c != '\t') || c > 126) invalid = true;
                text[length++] = static_cast<char>(c);
            }
        }
        return Result::Pending;
    }

    void reset() { length = 0; overflow = invalid = false; text[0] = '\0'; }
};

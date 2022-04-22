#include "sh61.hh"
#include <cctype>
#include <cstring>
#include <sstream>

// isshellspecial(ch)
//    Test if `ch` is a command that's special to the shell (that ends
//    a command word).

inline bool isshellspecial(int ch) {
    return ch == '<' || ch == '>' || ch == '&' || ch == '|' || ch == ';'
        || ch == '(' || ch == ')' || ch == '#';
}


shell_parser::shell_parser(const char* str)
    : _str(str), _estr(str + strlen(str)) {
    while (isspace((unsigned char) *_str)) {
        ++_str;
    }
    if (*_str == '#') {
        _str = _estr;
    }
}

void shell_token_iterator::update() {
    // Skip initial spaces
    while (isspace((unsigned char) *_s)) {
        ++_s;
    }
    // Skip to end of line if comment
    if (*_s == '#') {
        _s += strlen(_s);
    }

    _len = 0;
    _quoted = false;

    // add new operators: &>, >&, &>>, >>&, 2>&1
    const std::string s2 = std::string(_s, 2);
    const std::string s3 = std::string(_s, 3);
    if ((s2 == "&>" && _s[2] != '>') || s2 == ">&") {
        _len = 2;
        _type = TYPE_REDIRECT_OP;
        return;
    }
    if (s2 == "|&") {
        _len = 2;
        _type = TYPE_PIPE;
        return;
    }
    if (s3 == "&>>" || s3 == ">>&") {
        _len = 3;
        _type = TYPE_REDIRECT_OP;
        return;
    }
    if (std::string(_s, 4) == "2>&1") {
        _len = 4;
        _type = TYPE_REDIRECT_OP;
        return;
    }
    // the original code can handle <, >, 2>, >>, 2>> operators

    // Read token starting at _s, setting _type, _len, and _quoted.
    // - _len: Length of token in characters.
    // - _type: Type of token (one of the TYPE_ constants).
    // - _quoted: True iff this token contains quotes or escapes.
    while (isdigit((unsigned char) _s[_len])) {
        ++_len;
    }
    if (_s[_len] == '<' || _s[_len] == '>') {
        // Redirection
        ++_len;
        if (_s[_len] == '>') {
            ++_len;
        } else {
            while (isdigit((unsigned char) _s[_len])) {
                ++_len;
            }
        }
        _type = TYPE_REDIRECT_OP;

    } else if (_len == 0
               && (_s[0] == '&' || _s[0] == '|')
               && _s[1] == _s[0]) {
        // Two-character operator
        _len = 2;
        _type = (_s[0] == '&' ? TYPE_AND : TYPE_OR);

    } else if (_len == 0
               && isshellspecial((unsigned char) _s[0])) {
        // One-character operator
        _len = 1;
        if (_s[0] == ';') {
            _type = TYPE_SEQUENCE;
        } else if (_s[0] == '&') {
            _type = TYPE_BACKGROUND;
        } else if (_s[0] == '|') {
            _type = TYPE_PIPE;
        } else if (_s[0] == '(') {
            _type = TYPE_LPAREN;
        } else if (_s[0] == ')') {
            _type = TYPE_RPAREN;
        } else {
            _type = TYPE_OTHER;
        }

    } else if (_len == 0
               && _s[0] == '\0') {
        // End of string
        _type = TYPE_SEQUENCE;

    } else {
        // Ordinary word (command, argument, or filename)
        _type = TYPE_NORMAL;
        int curquote = 0;
        // Read characters up to the end of the token.
        while ((_s[_len] && curquote)
               || (_s[_len] != '\0'
                   && !isspace((unsigned char) _s[_len])
                   && !isshellspecial((unsigned char) _s[_len]))) {
            if ((_s[_len] == '\"' || _s[_len] == '\'') && !curquote) {
                curquote = _s[_len];
                _quoted = true;
            } else if (_s[_len] == curquote) {
                curquote = 0;
            } else if (_s[_len] == '\\'
                       && _s[_len+1] != '\0'
                       && curquote != '\'') {
                _quoted = true;
                ++_len;
            }
            ++_len;
        }
    }
}

std::string shell_token_iterator::str() const {
    if (!_quoted) {
        return std::string(_s, _len);
    } else {
        std::ostringstream build;
        int curquote = 0;
        for (unsigned pos = 0; pos != _len; ++pos) {
            if ((_s[pos] == '\"' || _s[pos] == '\'') && !curquote) {
                curquote = _s[pos];
            } else if (_s[pos] == curquote) {
                curquote = 0;
            } else if (_s[pos] == '\\'
                       && _s[pos+1] != '\0'
                       && curquote != '\'') {
                build << _s[pos+1];
                ++pos;
            } else {
                build << _s[pos];
            }
        }
        return build.str();
    }
}

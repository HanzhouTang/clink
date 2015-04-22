/* Copyright (c) 2015 Martin Ridgers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pch.h"
#include "ecma48_terminal.h"
#include "shared/util.h"

//------------------------------------------------------------------------------
static unsigned g_last_buffer_size = 0;

int             get_clink_setting_int(const char*);
void            on_terminal_resize();
const wchar_t*  find_next_ansi_code_w(const wchar_t*, int*);
int             parse_ansi_code_w(const wchar_t*, int*, int);



//------------------------------------------------------------------------------
static int sgr_to_attr(int colour)
{
    static const int map[] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    return map[colour & 7];
}

//------------------------------------------------------------------------------
static int fwrite_sgr_code(const wchar_t* code, int current, int defaults)
{
    int params[32];
    int i = parse_ansi_code_w(code, params, sizeof_array(params));
    if (i != 'm')
        return current;

    // Count the number of parameters the code has.
    int n = 0;
    while (n < sizeof_array(params) && params[n] >= 0)
        ++n;

    // Process each code that is supported.
    int attr = current;
    for (i = 0; i < n; ++i)
    {
        int param = params[i];

        if (param == 0) // reset
        {
            attr = defaults;
        }
        else if (param == 1) // fg intensity (bright)
        {
            attr |= 0x08;
        }
        else if (param == 2 || param == 22) // fg intensity (normal)
        {
            attr &= ~0x08;
        }
        else if (param == 4) // bg intensity (bright)
        {
            attr |= 0x80;
        }
        else if (param == 24) // bg intensity (normal)
        {
            attr &= ~0x80;
        }
        else if ((unsigned int)param - 30 < 8) // fg colour
        {
            attr = (attr & 0xf8) | sgr_to_attr(param - 30);
        }
        else if (param == 39) // default fg colour
        {
            attr = (attr & 0xf8) | (defaults & 0x07);
        }
        else if ((unsigned int)param - 40 < 8) // bg colour
        {
            attr = (attr & 0x8f) | (sgr_to_attr(param - 40) << 4);
        }
        else if (param == 49) // default bg colour
        {
            attr = (attr & 0x8f) | (defaults & 0x70);
        }
        else if (param == 38 || param == 48) // extended colour (skipped)
        {
            // format = param;5;[0-255] or param;2;r;g;b
            ++i;
            if (i >= n)
                break;

            switch (params[i])
            {
            case 2: i += 3; break;
            case 5: i += 1; break;
            }

            continue;
        }
    }

    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(handle, attr);
    return attr;
}



//------------------------------------------------------------------------------
ecma48_terminal::ecma48_terminal()
: m_enable_sgr(true)
{
}

//------------------------------------------------------------------------------
ecma48_terminal::~ecma48_terminal()
{
}

//------------------------------------------------------------------------------
int ecma48_terminal::read()
{
    static int       carry        = 0; // Multithreading? What's that?
    static const int CTRL_PRESSED = LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED;

    // Clear all flags so the console doesn't do anything special. This prevents
    // key presses such as Ctrl-C and Ctrl-S from being swallowed.
    HANDLE handle_stdin = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleMode(handle_stdin, ENABLE_WINDOW_INPUT);

loop:
    int key_char = 0;
    int key_vk = 0;
    int key_sc = 0;
    int key_flags = 0;
    int alt = 0;

    // Read a key or use what was carried across from a previous call.
    if (carry)
    {
        key_flags = ENHANCED_KEY;
        key_char = carry;
        carry = 0;
    }
    else
    {
        HANDLE handle_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        DWORD i;
        INPUT_RECORD record;
        const KEY_EVENT_RECORD* key;
        int altgr_sub;

        GetConsoleScreenBufferInfo(handle_stdout, &csbi);

        // Check for a new buffer size for simulated SIGWINCH signals.
        i = (csbi.dwSize.X << 16);
        i |= (csbi.srWindow.Bottom - csbi.srWindow.Top) + 1;
        if (!g_last_buffer_size || g_last_buffer_size != i)
        {
            if (g_last_buffer_size)
                on_terminal_resize();

            g_last_buffer_size = i;
            goto loop;
        }

        // Fresh read from the console.
        ReadConsoleInputW(handle_stdin, &record, 1, &i);
        if (record.EventType != KEY_EVENT)
            goto loop;

        GetConsoleScreenBufferInfo(handle_stdout, &csbi);
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT)
        {
            on_terminal_resize();

            g_last_buffer_size = (csbi.dwSize.X << 16) | csbi.dwSize.Y;
            goto loop;
        }

        key = &record.Event.KeyEvent;
        key_char = key->uChar.UnicodeChar;
        key_vk = key->wVirtualKeyCode;
        key_sc = key->wVirtualScanCode;
        key_flags = key->dwControlKeyState;

#if defined(DEBUG_GETC) && defined(_DEBUG)
        {
            static int id = 0;
            int i;
            printf("\n%03d: %s ", id++, key->bKeyDown ? "+" : "-");
            for (i = 2; i < sizeof(*key) / sizeof(short); ++i)
            {
                printf("%04x ", ((unsigned short*)key)[i]);
            }
        }
#endif

        if (key->bKeyDown == FALSE)
        {
            // Some times conhost can send through ALT codes, with the resulting
            // Unicode code point in the Alt key-up event.
            if (key_vk == VK_MENU && key_char)
            {
                goto end;
            }

            goto loop;
        }

        // Windows supports an AltGr substitute which we check for here. As it
        // collides with Readline mappings Clink's support can be disabled.
        altgr_sub = !!(key_flags & LEFT_ALT_PRESSED);
        altgr_sub &= !!(key_flags & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED));
        altgr_sub &= !!key_char;

        if (altgr_sub && !get_clink_setting_int("use_altgr_substitute"))
        {
            altgr_sub = 0;
            key_char = 0;
        }

        if (!altgr_sub)
            alt = !!(key_flags & LEFT_ALT_PRESSED);
    }

    // No Unicode character? Then some post-processing is required to make the
    // output compatible with whatever standard Linux terminals adhere to and
    // that which Readline expects.
    if (key_char == 0)
    {
        int i;

        // The numpad keys such as PgUp, End, etc. don't come through with the
        // ENHANCED_KEY flag set so we'll infer it here.
        static const int enhanced_vks[] = {
            VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_HOME, VK_END,
            VK_INSERT, VK_DELETE, VK_PRIOR, VK_NEXT,
        };

        for (i = 0; i < sizeof_array(enhanced_vks); ++i)
        {
            if (key_vk == enhanced_vks[i])
            {
                key_flags |= ENHANCED_KEY;
                break;
            }
        }

        // Differentiate enhanced keys depending on modifier key state. MSVC's
        // runtime does something similar. Slightly non-standard.
        if (key_flags & ENHANCED_KEY)
        {
            static const int mod_map[][4] =
            {
                //Nrml  Shft  Ctrl  CtSh
                { 0x47, 0x61, 0x77, 0x21 }, // Gaw! home
                { 0x48, 0x62, 0x54, 0x22 }, // HbT" up
                { 0x49, 0x63, 0x55, 0x23 }, // IcU# pgup
                { 0x4b, 0x64, 0x73, 0x24 }, // Kds$ left
                { 0x4d, 0x65, 0x74, 0x25 }, // Met% right
                { 0x4f, 0x66, 0x75, 0x26 }, // Ofu& end
                { 0x50, 0x67, 0x56, 0x27 }, // PgV' down
                { 0x51, 0x68, 0x76, 0x28 }, // Qhv( pgdn
                { 0x52, 0x69, 0x57, 0x29 }, // RiW) insert
                { 0x53, 0x6a, 0x58, 0x2a }, // SjX* delete
            };

            for (i = 0; i < sizeof_array(mod_map); ++i)
            {
                int j = 0;
                if (mod_map[i][j] != key_sc)
                {
                    continue;
                }

                j += !!(key_flags & SHIFT_PRESSED);
                j += !!(key_flags & CTRL_PRESSED) << 1;
                carry = mod_map[i][j];
                break;
            }

            // Blacklist.
            if (!carry)
            {
                goto loop;
            }

            key_vk = 0xe0;
        }
        else if (!(key_flags & CTRL_PRESSED))
        {
            goto loop;
        }

        // This builds Ctrl-<key> map to match that as described by Readline's
        // source for the emacs/vi keymaps.
        #define CONTAINS(l, r) (unsigned)(key_vk - l) <= (r - l)
        else if (CONTAINS('A', 'Z'))    key_vk -= 'A' - 1;
        else if (CONTAINS(0xdb, 0xdd))  key_vk -= 0xdb - 0x1b;
        else if (key_vk == 0x32)        key_vk = 0;
        else if (key_vk == 0x36)        key_vk = 0x1e;
        else if (key_vk == 0xbd)        key_vk = 0x1f;
        else                            goto loop;
        #undef CONTAINS

        key_char = key_vk;
    }
    else if (!(key_flags & ENHANCED_KEY) && key_char > 0x7f)
    {
        key_char |= 0x8000000;
    }

    // Special case for shift-tab.
    if (key_char == '\t' && !carry && (key_flags & SHIFT_PRESSED))
    {
        key_char = 0xe0;
        carry = 'Z';
    }

end:
#if defined(DEBUG_GETC) && defined(_DEBUG)
    printf("\n%08x '%c'", key_char, key_char);
#endif

    return key_char;
}

//------------------------------------------------------------------------------
void ecma48_terminal::write(const wchar_t* chars, int char_count)
{
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);

    if (!m_enable_sgr)
    {
        DWORD written;
        WriteConsoleW(handle, chars, char_count, &written, nullptr);
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(handle, &csbi);

    int attr_def = csbi.wAttributes;
    int attr_cur = attr_def;
    const wchar_t* next = chars;
    while (*next)
    {
        int ansi_size;
        const wchar_t* ansi_code = find_next_ansi_code_w(next, &ansi_size);

        // Dispatch console write
        DWORD written;
        WriteConsoleW(handle, next, ansi_code - next, &written, nullptr);

        // Process ansi code.
        if (*ansi_code)
            attr_cur = fwrite_sgr_code(ansi_code, attr_cur, attr_def);

        next = ansi_code + ansi_size;
    }

    SetConsoleTextAttribute(handle, attr_def);
}

//------------------------------------------------------------------------------
void ecma48_terminal::flush()
{
    // When writing to the console conhost.exe will restart the cursor blink
    // timer and hide it which can be disorientating, especially when moving
    // around a line. The below will make sure it stays visible.
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(handle, &csbi);
    SetConsoleCursorPosition(handle, csbi.dwCursorPosition);
}

//------------------------------------------------------------------------------
void ecma48_terminal::check_sgr_support()
{
    // Check for the presence of known third party tools that also provide ANSI
    // escape code support.
    const char* dll_names[] = {
        "conemuhk.dll",
        "conemuhk64.dll",
        "ansi.dll",
        "ansi32.dll",
        "ansi64.dll",
    };

    for (int i = 0; i < sizeof_array(dll_names); ++i)
    {
        const char* dll_name = dll_names[i];
        if (GetModuleHandle(dll_name) != nullptr)
        {
            LOG_INFO("Disabling ANSI support. Found '%s'", dll_name);
            m_enable_sgr = false;
            return;
        }
    }

    // Give the user the option to disable ANSI support.
    if (get_clink_setting_int("ansi_code_support") == 0)
        m_enable_sgr = false;

    return;
}
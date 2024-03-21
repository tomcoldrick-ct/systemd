/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "glyph-util.h"
#include "sysupdate-update-set-flags.h"
#include "terminal-util.h"

const char *update_set_flags_to_color(UpdateSetFlags flags) {

        if (flags == 0 || (flags & UPDATE_OBSOLETE))
                return (flags & UPDATE_NEWEST) ? ansi_highlight_grey() : ansi_grey();

        if (FLAGS_SET(flags, UPDATE_INSTALLED|UPDATE_NEWEST))
                return ansi_highlight();

        if (FLAGS_SET(flags, UPDATE_INSTALLED|UPDATE_PROTECTED))
                return ansi_highlight_magenta();

        if ((flags & (UPDATE_AVAILABLE|UPDATE_INSTALLED|UPDATE_NEWEST|UPDATE_OBSOLETE)) == (UPDATE_AVAILABLE|UPDATE_NEWEST))
                return ansi_highlight_green();

        return NULL;
}

const char *update_set_flags_to_glyph(UpdateSetFlags flags) {

        if (flags == 0 || (flags & UPDATE_OBSOLETE))
                return special_glyph(SPECIAL_GLYPH_MULTIPLICATION_SIGN);

        if (FLAGS_SET(flags, UPDATE_INSTALLED|UPDATE_NEWEST))
                return special_glyph(SPECIAL_GLYPH_BLACK_CIRCLE);

        if (FLAGS_SET(flags, UPDATE_INSTALLED|UPDATE_PROTECTED))
                return special_glyph(SPECIAL_GLYPH_WHITE_CIRCLE);

        if ((flags & (UPDATE_AVAILABLE|UPDATE_INSTALLED|UPDATE_NEWEST|UPDATE_OBSOLETE)) == (UPDATE_AVAILABLE|UPDATE_NEWEST))
                return special_glyph(SPECIAL_GLYPH_CIRCLE_ARROW);

        return " ";
}

const char *update_set_flags_to_string(UpdateSetFlags flags) {

        switch ((unsigned) flags) {

        case 0:
                return "n/a";

        case UPDATE_INSTALLED|UPDATE_NEWEST:
        case UPDATE_INSTALLED|UPDATE_NEWEST|UPDATE_PROTECTED:
        case UPDATE_INSTALLED|UPDATE_AVAILABLE|UPDATE_NEWEST:
        case UPDATE_INSTALLED|UPDATE_AVAILABLE|UPDATE_NEWEST|UPDATE_PROTECTED:
                return "current";

        case UPDATE_AVAILABLE|UPDATE_NEWEST:
        case UPDATE_AVAILABLE|UPDATE_NEWEST|UPDATE_PROTECTED:
                return "candidate";

        case UPDATE_INSTALLED:
        case UPDATE_INSTALLED|UPDATE_AVAILABLE:
                return "installed";

        case UPDATE_INSTALLED|UPDATE_PROTECTED:
        case UPDATE_INSTALLED|UPDATE_AVAILABLE|UPDATE_PROTECTED:
                return "protected";

        case UPDATE_AVAILABLE:
        case UPDATE_AVAILABLE|UPDATE_PROTECTED:
                return "available";

        case UPDATE_INSTALLED|UPDATE_OBSOLETE|UPDATE_NEWEST:
        case UPDATE_INSTALLED|UPDATE_OBSOLETE|UPDATE_NEWEST|UPDATE_PROTECTED:
        case UPDATE_INSTALLED|UPDATE_AVAILABLE|UPDATE_OBSOLETE|UPDATE_NEWEST:
        case UPDATE_INSTALLED|UPDATE_AVAILABLE|UPDATE_OBSOLETE|UPDATE_NEWEST|UPDATE_PROTECTED:
                return "current+obsolete";

        case UPDATE_INSTALLED|UPDATE_OBSOLETE:
        case UPDATE_INSTALLED|UPDATE_AVAILABLE|UPDATE_OBSOLETE:
                return "installed+obsolete";

        case UPDATE_INSTALLED|UPDATE_OBSOLETE|UPDATE_PROTECTED:
        case UPDATE_INSTALLED|UPDATE_AVAILABLE|UPDATE_OBSOLETE|UPDATE_PROTECTED:
                return "protected+obsolete";

        case UPDATE_AVAILABLE|UPDATE_OBSOLETE:
        case UPDATE_AVAILABLE|UPDATE_OBSOLETE|UPDATE_PROTECTED:
        case UPDATE_AVAILABLE|UPDATE_OBSOLETE|UPDATE_NEWEST:
        case UPDATE_AVAILABLE|UPDATE_OBSOLETE|UPDATE_NEWEST|UPDATE_PROTECTED:
                return "available+obsolete";

        default:
                assert_not_reached();
        }
}

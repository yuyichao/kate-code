/**
 * TerminalRenderer - Minimal ANSI terminal emulator for rendering CLI output
 *
 * Handles:
 * - Cursor positioning (CSI A/B/C/D/H/G/d/E/F)
 * - Screen/line clearing (CSI J/K)
 * - SGR colors (16 basic, 256-color, 24-bit RGB)
 * - Text attributes (bold, underline, reverse)
 *
 * Based on turbogo/terminal Go implementation.
 */
class TerminalRenderer {
    constructor(cols = 120, rows = 50) {
        this.cols = cols;
        this.rows = rows;
        this.cursorX = 0;
        this.cursorY = 0;

        // Cell buffer - 2D array of {char, fg, bg, bold, underline, reverse}
        this.cells = this.createBuffer(cols, rows);

        // Current text attributes
        this.currentFg = null;      // null = default
        this.currentBg = null;      // null = default
        this.bold = false;
        this.underline = false;
        this.reverse = false;

        // Parser state machine
        this.state = 'normal';      // normal, escape, csi, osc
        this.csiParams = '';        // Accumulated CSI parameters
        this.oscData = '';          // Accumulated OSC data
    }

    createBuffer(cols, rows) {
        const buffer = [];
        for (let y = 0; y < rows; y++) {
            buffer.push(this.createRow(cols));
        }
        return buffer;
    }

    createRow(cols) {
        const row = [];
        for (let x = 0; x < cols; x++) {
            row.push(this.createCell());
        }
        return row;
    }

    createCell() {
        return {
            char: ' ',
            fg: null,
            bg: null,
            bold: false,
            underline: false,
            reverse: false
        };
    }

    /**
     * Process terminal output and update internal buffer. The text is
     * already decoded (a JavaScript string), so it is read by code point.
     */
    write(text) {
        for (const char of text) {
            this.processByte(char.codePointAt(0), char);
        }
    }

    processByte(byte, char) {
        switch (this.state) {
            case 'normal':
                this.processNormal(byte, char);
                break;
            case 'escape':
                this.processEscape(byte, char);
                break;
            case 'csi':
                this.processCSI(byte, char);
                break;
            case 'osc':
                this.processOSC(byte, char);
                break;
        }
    }

    processNormal(byte, char) {
        if (byte === 0x1B) {
            // ESC - start escape sequence
            this.state = 'escape';
        } else if (byte === 0x0D) {
            // CR - carriage return
            this.cursorX = 0;
        } else if (byte === 0x0A) {
            // LF - line feed (treat as CR+LF for typical terminal output)
            this.cursorX = 0;
            this.lineFeed();
        } else if (byte === 0x08) {
            // BS - backspace
            if (this.cursorX > 0) {
                this.cursorX--;
            }
        } else if (byte === 0x09) {
            // TAB - move to next tab stop (every 8 columns)
            this.cursorX = Math.min(this.cols - 1, (Math.floor(this.cursorX / 8) + 1) * 8);
        } else if (byte >= 0x20 && byte !== 0x7F && !(byte >= 0x80 && byte <= 0x9F)) {
            // Printable (C0/C1 controls and DEL are not)
            this.writeChar(char);
        }
    }

    processEscape(byte, char) {
        if (byte === 0x5B) {
            // '[' - CSI
            this.state = 'csi';
            this.csiParams = '';
        } else if (byte === 0x5D) {
            // ']' - OSC
            this.state = 'osc';
            this.oscData = '';
        } else if (byte === 0x37) {
            // '7' - Save cursor (DECSC)
            this.savedCursorX = this.cursorX;
            this.savedCursorY = this.cursorY;
            this.state = 'normal';
        } else if (byte === 0x38) {
            // '8' - Restore cursor (DECRC)
            if (this.savedCursorX !== undefined) {
                this.cursorX = this.savedCursorX;
                this.cursorY = this.savedCursorY;
            }
            this.state = 'normal';
        } else if (byte === 0x44) {
            // 'D' - Index (move down, scroll if needed)
            this.lineFeed();
            this.state = 'normal';
        } else if (byte === 0x4D) {
            // 'M' - Reverse Index (move up, scroll if needed)
            if (this.cursorY > 0) {
                this.cursorY--;
            }
            this.state = 'normal';
        } else if (byte === 0x45) {
            // 'E' - Next Line
            this.cursorX = 0;
            this.lineFeed();
            this.state = 'normal';
        } else if (byte === 0x63) {
            // 'c' - Reset to Initial State (RIS)
            this.reset();
            this.state = 'normal';
        } else {
            // Unknown escape, return to normal
            this.state = 'normal';
        }
    }

    processCSI(byte, char) {
        if (byte >= 0x30 && byte <= 0x3F) {
            // Parameter bytes (0-9, ;, <, =, >, ?)
            this.csiParams += char;
        } else if (byte >= 0x40 && byte <= 0x7E) {
            // Final byte - execute command
            this.executeCSI(char);
            this.state = 'normal';
        } else {
            // Invalid, return to normal
            this.state = 'normal';
        }
    }

    processOSC(byte, char) {
        if (byte === 0x07) {
            // BEL - terminates OSC
            this.state = 'normal';
        } else if (byte === 0x1B) {
            // Could be ST (ESC \)
            // For simplicity, just end OSC
            this.state = 'normal';
        } else {
            this.oscData += char;
        }
    }

    executeCSI(finalByte) {
        const params = this.parseCSIParams(this.csiParams);

        switch (finalByte) {
            case 'A': // Cursor Up
                this.cursorY = Math.max(0, this.cursorY - (params[0] || 1));
                break;

            case 'B': // Cursor Down
                this.cursorY = Math.min(this.rows - 1, this.cursorY + (params[0] || 1));
                break;

            case 'C': // Cursor Forward
                this.cursorX = Math.min(this.cols - 1, this.cursorX + (params[0] || 1));
                break;

            case 'D': // Cursor Backward
                this.cursorX = Math.max(0, this.cursorX - (params[0] || 1));
                break;

            case 'E': // Cursor Next Line
                this.cursorX = 0;
                this.cursorY = Math.min(this.rows - 1, this.cursorY + (params[0] || 1));
                break;

            case 'F': // Cursor Previous Line
                this.cursorX = 0;
                this.cursorY = Math.max(0, this.cursorY - (params[0] || 1));
                break;

            case 'G': // Cursor Horizontal Absolute
                this.cursorX = Math.min(this.cols - 1, Math.max(0, (params[0] || 1) - 1));
                break;

            case 'H': // Cursor Position
            case 'f': // Horizontal and Vertical Position
                this.cursorY = Math.min(this.rows - 1, Math.max(0, (params[0] || 1) - 1));
                this.cursorX = Math.min(this.cols - 1, Math.max(0, (params[1] || 1) - 1));
                break;

            case 'd': // Vertical Position Absolute
                this.cursorY = Math.min(this.rows - 1, Math.max(0, (params[0] || 1) - 1));
                break;

            case 'J': // Erase in Display
                this.eraseInDisplay(params[0] || 0);
                break;

            case 'K': // Erase in Line
                this.eraseInLine(params[0] || 0);
                break;

            case 'm': // SGR - Select Graphic Rendition
                this.processSGR(params);
                break;

            case 's': // Save Cursor Position
                this.savedCursorX = this.cursorX;
                this.savedCursorY = this.cursorY;
                break;

            case 'u': // Restore Cursor Position
                if (this.savedCursorX !== undefined) {
                    this.cursorX = this.savedCursorX;
                    this.cursorY = this.savedCursorY;
                }
                break;

            case 'L': // Insert Lines
            case 'M': // Delete Lines
            case '@': // Insert Characters
            case 'P': // Delete Characters
            case 'X': // Erase Characters
            case 'h': // Set Mode
            case 'l': // Reset Mode
            case 'r': // Set Scrolling Region
            case 'n': // Device Status Report
            case 'c': // Device Attributes
                // Ignore these for minimal implementation
                break;
        }
    }

    parseCSIParams(paramStr) {
        if (!paramStr) return [];

        // Handle private mode prefix (e.g., "?25" for cursor visibility)
        const cleaned = paramStr.replace(/^[?>=]/, '');

        return cleaned.split(';').map(p => {
            const n = parseInt(p, 10);
            return isNaN(n) ? 0 : n;
        });
    }

    processSGR(params) {
        if (params.length === 0) {
            params = [0];
        }

        let i = 0;
        while (i < params.length) {
            const code = params[i];

            if (code === 0) {
                // Reset
                this.currentFg = null;
                this.currentBg = null;
                this.bold = false;
                this.underline = false;
                this.reverse = false;
            } else if (code === 1) {
                this.bold = true;
            } else if (code === 4) {
                this.underline = true;
            } else if (code === 7) {
                this.reverse = true;
            } else if (code === 22) {
                this.bold = false;
            } else if (code === 24) {
                this.underline = false;
            } else if (code === 27) {
                this.reverse = false;
            } else if (code >= 30 && code <= 37) {
                // Standard foreground colors
                this.currentFg = code - 30;
            } else if (code === 38) {
                // Extended foreground color
                if (params[i + 1] === 5 && params.length > i + 2) {
                    // 256-color: 38;5;n
                    this.currentFg = { type: '256', value: params[i + 2] };
                    i += 2;
                } else if (params[i + 1] === 2 && params.length > i + 4) {
                    // 24-bit RGB: 38;2;r;g;b
                    this.currentFg = { type: 'rgb', r: params[i + 2], g: params[i + 3], b: params[i + 4] };
                    i += 4;
                }
            } else if (code === 39) {
                // Default foreground
                this.currentFg = null;
            } else if (code >= 40 && code <= 47) {
                // Standard background colors
                this.currentBg = code - 40;
            } else if (code === 48) {
                // Extended background color
                if (params[i + 1] === 5 && params.length > i + 2) {
                    // 256-color: 48;5;n
                    this.currentBg = { type: '256', value: params[i + 2] };
                    i += 2;
                } else if (params[i + 1] === 2 && params.length > i + 4) {
                    // 24-bit RGB: 48;2;r;g;b
                    this.currentBg = { type: 'rgb', r: params[i + 2], g: params[i + 3], b: params[i + 4] };
                    i += 4;
                }
            } else if (code === 49) {
                // Default background
                this.currentBg = null;
            } else if (code >= 90 && code <= 97) {
                // Bright foreground colors
                this.currentFg = code - 90 + 8;
            } else if (code >= 100 && code <= 107) {
                // Bright background colors
                this.currentBg = code - 100 + 8;
            }

            i++;
        }
    }

    eraseInDisplay(mode) {
        switch (mode) {
            case 0: // Cursor to end
                // Clear from cursor to end of line
                for (let x = this.cursorX; x < this.cols; x++) {
                    this.cells[this.cursorY][x] = this.createCell();
                }
                // Clear all lines below
                for (let y = this.cursorY + 1; y < this.rows; y++) {
                    this.cells[y] = this.createRow(this.cols);
                }
                break;

            case 1: // Start to cursor
                // Clear from start of line to cursor
                for (let x = 0; x <= this.cursorX; x++) {
                    this.cells[this.cursorY][x] = this.createCell();
                }
                // Clear all lines above
                for (let y = 0; y < this.cursorY; y++) {
                    this.cells[y] = this.createRow(this.cols);
                }
                break;

            case 2: // Entire screen
            case 3: // Entire screen + scrollback
                this.cells = this.createBuffer(this.cols, this.rows);
                break;
        }
    }

    eraseInLine(mode) {
        switch (mode) {
            case 0: // Cursor to end
                for (let x = this.cursorX; x < this.cols; x++) {
                    this.cells[this.cursorY][x] = this.createCell();
                }
                break;

            case 1: // Start to cursor
                for (let x = 0; x <= this.cursorX; x++) {
                    this.cells[this.cursorY][x] = this.createCell();
                }
                break;

            case 2: // Entire line
                this.cells[this.cursorY] = this.createRow(this.cols);
                break;
        }
    }

    writeChar(char) {
        const width = TerminalRenderer.charWidth(char.codePointAt(0));
        if (width === 0) {
            // Combining mark, zero-width character: joins the previous cell
            const x = this.cursorX > 0 ? this.cursorX - 1 : -1;
            if (x >= 0 && this.cursorY < this.rows) {
                const cell = this.cells[this.cursorY][x].char === ''
                    ? this.cells[this.cursorY][x - 1] : this.cells[this.cursorY][x];
                if (cell) {
                    cell.char += char;
                }
            }
            return;
        }
        // A wide character does not fit in the last column: wrap first
        if (width === 2 && this.cursorX >= this.cols - 1) {
            this.cursorX = 0;
            this.lineFeed();
        }
        // Expand buffer if needed
        while (this.cursorY >= this.rows) {
            this.cells.push(this.createRow(this.cols));
            this.rows++;
        }

        const row = this.cells[this.cursorY];
        for (let x = this.cursorX; x < this.cursorX + width; x++) {
            this.clearWideHalves(row, x);
        }
        const attrs = {
            fg: this.currentFg,
            bg: this.currentBg,
            bold: this.bold,
            underline: this.underline,
            reverse: this.reverse
        };
        // Write character with current attributes; a wide one takes the next
        // cell too (an empty continuation cell)
        row[this.cursorX] = Object.assign({ char: char }, attrs);
        if (width === 2) {
            row[this.cursorX + 1] = Object.assign({ char: '' }, attrs);
        }

        // Advance cursor
        this.cursorX += width;
        if (this.cursorX >= this.cols) {
            this.cursorX = 0;
            this.lineFeed();
        }
    }

    // Overwriting either half of a wide character blanks the other half.
    clearWideHalves(row, x) {
        if (row[x].char === '' && x > 0) {
            row[x - 1] = Object.assign({}, row[x - 1], { char: ' ' });
        }
        if (x + 1 < row.length && row[x + 1].char === '') {
            row[x + 1] = Object.assign({}, row[x + 1], { char: ' ' });
        }
    }

    /**
     * Columns a character takes in a terminal (as wcwidth): 0 for combining
     * marks and zero-width characters, 2 for East Asian wide/fullwidth
     * characters and emoji shown as emoji, else 1.
     */
    static charWidth(code) {
        if (code < 0x300) {
            return 1;
        }
        const char = String.fromCodePoint(code);
        if (/[\p{Mn}\p{Me}\p{Cf}]/u.test(char) || (code >= 0x1160 && code <= 0x11FF)) {
            return 0;  // combining marks, format characters, Hangul medial/final jamo
        }
        if ((code >= 0x1100 && code <= 0x115F)
            || code === 0x2329 || code === 0x232A
            || (code >= 0x2E80 && code <= 0xA4CF && code !== 0x303F)
            || (code >= 0xAC00 && code <= 0xD7A3)
            || (code >= 0xF900 && code <= 0xFAFF)
            || (code >= 0xFE10 && code <= 0xFE19)
            || (code >= 0xFE30 && code <= 0xFE6F)
            || (code >= 0xFF00 && code <= 0xFF60)
            || (code >= 0xFFE0 && code <= 0xFFE6)
            || (code >= 0x20000 && code <= 0x3FFFD)
            || /\p{Emoji_Presentation}/u.test(char)) {
            return 2;
        }
        return 1;
    }

    lineFeed() {
        this.cursorY++;
        // Expand buffer if needed (don't scroll for static rendering)
        while (this.cursorY >= this.rows) {
            this.cells.push(this.createRow(this.cols));
            this.rows++;
        }
    }

    reset() {
        this.cursorX = 0;
        this.cursorY = 0;
        this.currentFg = null;
        this.currentBg = null;
        this.bold = false;
        this.underline = false;
        this.reverse = false;
        this.cells = this.createBuffer(this.cols, this.rows);
    }

    /**
     * Render the buffer to HTML
     */
    toHtml() {
        const lines = [];

        // Find actual content bounds (trim empty rows from bottom)
        // Check each row for any non-empty content
        let maxRow = 0;
        for (let y = 0; y < this.rows; y++) {
            let hasContent = false;
            for (let x = 0; x < this.cols; x++) {
                if (this.cells[y][x].char !== ' ' ||
                    this.cells[y][x].fg !== null ||
                    this.cells[y][x].bg !== null) {
                    hasContent = true;
                    break;
                }
            }
            if (hasContent) {
                maxRow = y;
            }
        }

        for (let y = 0; y <= maxRow; y++) {
            let line = '';
            let currentSpan = null;
            let spanContent = '';

            // Find last non-space character in line
            let lineEnd = 0;
            for (let x = this.cols - 1; x >= 0; x--) {
                if (this.cells[y][x].char !== ' ' ||
                    this.cells[y][x].bg !== null) {
                    lineEnd = x + 1;
                    break;
                }
            }

            for (let x = 0; x < lineEnd; x++) {
                const cell = this.cells[y][x];
                const spanClass = this.getCellClass(cell);
                const spanStyle = this.getCellStyle(cell);
                const spanKey = spanClass + '|' + spanStyle;

                if (spanKey !== currentSpan) {
                    // Close previous span
                    if (currentSpan !== null && currentSpan !== '|') {
                        line += this.escapeHtml(spanContent) + '</span>';
                    } else if (spanContent) {
                        line += this.escapeHtml(spanContent);
                    }

                    // Start new span if needed
                    spanContent = '';
                    if (spanClass || spanStyle) {
                        let attrs = '';
                        if (spanClass) attrs += ` class="${spanClass}"`;
                        if (spanStyle) attrs += ` style="${spanStyle}"`;
                        line += `<span${attrs}>`;
                    }
                    currentSpan = spanKey;
                }

                spanContent += cell.char;
            }

            // Close final span
            if (currentSpan !== null && currentSpan !== '|') {
                line += this.escapeHtml(spanContent) + '</span>';
            } else if (spanContent) {
                line += this.escapeHtml(spanContent);
            }

            lines.push(line);
        }

        return lines.join('\n');
    }

    getCellClass(cell) {
        const classes = [];

        if (cell.bold) classes.push('ansi-bold');
        if (cell.underline) classes.push('ansi-underline');

        // Handle basic 16 colors with classes
        if (typeof cell.fg === 'number') {
            if (cell.fg < 8) {
                classes.push(['ansi-black', 'ansi-red', 'ansi-green', 'ansi-yellow',
                             'ansi-blue', 'ansi-magenta', 'ansi-cyan', 'ansi-white'][cell.fg]);
            } else {
                classes.push(['ansi-bright-black', 'ansi-bright-red', 'ansi-bright-green',
                             'ansi-bright-yellow', 'ansi-bright-blue', 'ansi-bright-magenta',
                             'ansi-bright-cyan', 'ansi-bright-white'][cell.fg - 8]);
            }
        }

        if (typeof cell.bg === 'number') {
            if (cell.bg < 8) {
                classes.push(['ansi-bg-black', 'ansi-bg-red', 'ansi-bg-green', 'ansi-bg-yellow',
                             'ansi-bg-blue', 'ansi-bg-magenta', 'ansi-bg-cyan', 'ansi-bg-white'][cell.bg]);
            } else {
                classes.push(['ansi-bg-bright-black', 'ansi-bg-bright-red', 'ansi-bg-bright-green',
                             'ansi-bg-bright-yellow', 'ansi-bg-bright-blue', 'ansi-bg-bright-magenta',
                             'ansi-bg-bright-cyan', 'ansi-bg-bright-white'][cell.bg - 8]);
            }
        }

        if (cell.reverse) classes.push('ansi-reverse');

        return classes.join(' ');
    }

    getCellStyle(cell) {
        const styles = [];

        // Handle 256-color and RGB with inline styles
        if (cell.fg && typeof cell.fg === 'object') {
            if (cell.fg.type === 'rgb') {
                styles.push(`color: rgb(${cell.fg.r}, ${cell.fg.g}, ${cell.fg.b})`);
            } else if (cell.fg.type === '256') {
                styles.push(`color: ${this.color256ToRgb(cell.fg.value)}`);
            }
        }

        if (cell.bg && typeof cell.bg === 'object') {
            if (cell.bg.type === 'rgb') {
                styles.push(`background-color: rgb(${cell.bg.r}, ${cell.bg.g}, ${cell.bg.b})`);
            } else if (cell.bg.type === '256') {
                styles.push(`background-color: ${this.color256ToRgb(cell.bg.value)}`);
            }
        }

        return styles.join('; ');
    }

    /**
     * Convert 256-color palette index to RGB
     */
    color256ToRgb(n) {
        if (n < 16) {
            // Standard colors - use CSS classes instead
            const colors = [
                '#000000', '#cd0000', '#00cd00', '#cdcd00',
                '#0000ee', '#cd00cd', '#00cdcd', '#e5e5e5',
                '#7f7f7f', '#ff0000', '#00ff00', '#ffff00',
                '#5c5cff', '#ff00ff', '#00ffff', '#ffffff'
            ];
            return colors[n];
        } else if (n < 232) {
            // 216-color cube: 6x6x6
            const index = n - 16;
            const r = Math.floor(index / 36);
            const g = Math.floor((index % 36) / 6);
            const b = index % 6;
            const toVal = v => v === 0 ? 0 : 55 + v * 40;
            return `rgb(${toVal(r)}, ${toVal(g)}, ${toVal(b)})`;
        } else {
            // Grayscale: 24 shades
            const gray = 8 + (n - 232) * 10;
            return `rgb(${gray}, ${gray}, ${gray})`;
        }
    }

    escapeHtml(text) {
        return text
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }
}

/**
 * Convenience function to render ANSI text to HTML
 */
function renderAnsiToHtml(text, cols = 120, rows = 50) {
    const renderer = new TerminalRenderer(cols, rows);
    renderer.write(text);
    return renderer.toHtml();
}

// Export for use in chat.js
if (typeof window !== 'undefined') {
    window.TerminalRenderer = TerminalRenderer;
    window.renderAnsiToHtml = renderAnsiToHtml;
}

#include "nfc_parser.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

void nfc_card_reset(nfc_ui_card_t *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
}

bool nfc_line_is_end(const char *line)
{
    if (!line) return false;
    return strcmp(line, "[NFC] END") == 0 || strcmp(line, "[NFC] END\r") == 0;
}

bool nfc_parse_card_line(const char *line, nfc_ui_card_t *c)
{
    if (!line || !c) return false;
    if (strncmp(line, "[NFC]", 5) != 0) return false;

    if (nfc_line_is_end(line))
        return true;

    if (strncmp(line, "[NFC] type: ", 12) == 0) {
        snprintf(c->type, sizeof(c->type), "%s", line + 12);
        return true;
    }
    if (strncmp(line, "[NFC] uid: ", 11) == 0) {
        snprintf(c->uid, sizeof(c->uid), "%s", line + 11);
        return true;
    }
    if (strncmp(line, "[NFC] atqa: ", 12) == 0) {
        unsigned a0 = 0, a1 = 0, sak = 0;
        if (sscanf(line, "[NFC] atqa: %02X %02X  sak: %02X", &a0, &a1, &sak) == 3) {
            c->atqa[0] = (uint8_t)a0;
            c->atqa[1] = (uint8_t)a1;
            c->sak = (uint8_t)sak;
            c->have_atqa_sak = true;
        }
        return true;
    }
    if (strncmp(line, "[NFC] idm: ", 11) == 0) {
        snprintf(c->idm, sizeof(c->idm), "%s", line + 11);
        c->have_idm = true;
        return true;
    }
    if (strncmp(line, "[NFC] data: ", 12) == 0) {
        if (sscanf(line, "[NFC] data: %u bytes, %u blocks x %u",
                   &c->data_len, &c->num_blocks, &c->block_size) >= 1) {
            c->have_data = true;
        }
        return true;
    }
    if (strstr(line, "[NFC] no card detected")) {
        c->no_card = true;
        return true;
    }
    if (strstr(line, "[NFC] not detected")) {
        c->not_detected = true;
        return true;
    }
    if (strstr(line, "[NFC] not initialized")) {
        c->not_initialized = true;
        return true;
    }
    if (strstr(line, "[NFC] nothing to save")) {
        c->nothing_to_save = true;
        return true;
    }
    if (strstr(line, "[NFC] emulate failed")) {
        c->emulate_failed = true;
        return true;
    }
    if (strstr(line, "[NFC] no card loaded")) {
        c->no_card_loaded = true;
        return true;
    }
    if (strncmp(line, "[NFC] saved: ", 13) == 0) {
        snprintf(c->saved_path, sizeof(c->saved_path), "%s", line + 13);
        c->have_saved = true;
        return true;
    }
    if (strncmp(line, "[NFC] emulating ", 16) == 0) {
        snprintf(c->emulating_summary, sizeof(c->emulating_summary), "%s", line + 16);
        /* Trim trailing " Use 'stop'..." if present */
        char *use = strstr(c->emulating_summary, ". Use ");
        if (use) *use = '\0';
        c->have_emulating = true;
        return true;
    }

    return true; /* other [NFC] lines still count as handled */
}

bool nfc_parse_list_entry(const char *line, nfc_list_entry_t *out)
{
    if (!line || !out) return false;
    if (strstr(line, "[NFC]")) return false;

    int idx = -1;
    char name[64];
    if (sscanf(line, "%d %63s", &idx, name) != 2) return false;
    if (idx < 0) return false;

    out->idx = idx;
    snprintf(out->name, sizeof(out->name), "%s", name);
    return true;
}

bool nfc_parse_card_count(const char *line, int *out_n)
{
    if (!line || !out_n) return false;
    int n = 0;
    if (sscanf(line, "[NFC] %d card(s)", &n) == 1) {
        *out_n = n;
        return true;
    }
    return false;
}

bool nfc_name_is_valid(const char *name)
{
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; p++) {
        if (isalnum((unsigned char)*p)) continue;
        if (*p == '_' || *p == '-' || *p == '.') continue;
        return false;
    }
    return true;
}

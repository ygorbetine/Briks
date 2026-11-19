/* QoL-3: "Enviar log": últimas linhas do rede.log SEM segredos, prontas para colar no suporte. Funções puras (testáveis sem janela).
 * O core nunca grava tokens nem IPs no rede.log; mesmo assim tudo que parece segredo é coberto aqui (defesa em profundidade):
 *   - palavras longas (>= 24 caracteres com letras e números), JWT (eyJ...), chaves hexadecimais/base64 -> [oculto]
 *   - valor depois de token/senha/password/secret/key/chave/auth/bearer/cookie (com = ou :) -> [oculto]
 *   - endereços IPv4/IPv6 -> [ip]; e-mails -> [email] */
#ifndef BRIKS_LOGNET_H
#define BRIKS_LOGNET_H
#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include <stdbool.h>
static inline bool lognet_wc(unsigned char c) { return isalnum(c) || c == '_' || c == '-' || c == '+' || c == '/' || c == '=' || c == '.' || c == ':' || c == '@' || c == '%'; }
static inline bool lognet_keyword(const char *w, size_t n) {
    static const char *K[] = { "token", "senha", "password", "passwd", "secret", "key", "chave", "auth", "bearer", "cookie", "credential", "apikey", "jwt", "sig", "signature" };
    char low[48]; if (n >= sizeof low) n = sizeof low - 1; for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)w[i]); low[n] = 0;
    for (size_t k = 0; k < sizeof K / sizeof *K; k++) if (strstr(low, K[k])) return true;
    return false;
}
/* decide o destino de uma palavra: 0 mantém; 1 [oculto]; 2 [ip]; 3 [email] */
static inline int lognet_class(const char *w, size_t n) {
    if (n >= 3 && !strncmp(w, "eyJ", 3)) return 1;
    int dig = 0, alp = 0, dots = 0, colons = 0, hex = 0, at = 0;
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)w[i]; if (isdigit(c)) dig++; else if (isalpha(c)) alp++; if (c == '.') dots++; if (c == ':') colons++; if (c == '@') at++; if (isxdigit(c) || c == ':' || c == '.') hex++; }
    if (at == 1 && dots >= 1 && n > 5 && alp > 0) return 3;
    if (dots == 3 && dig + dots >= (int)n - 2 && dig >= 4) return 2;
    if (colons >= 2 && hex == (int)n && dig >= 1 && n >= 5) return 2;
    if (n >= 24 && dig > 0 && alp > 0) return 1;
    if (n >= 32) return 1;
    return 0;
}
/* copia `in` para `out` cobrindo segredos; devolve o tamanho escrito (sempre terminado em 0) */
static inline size_t lognet_redact(const char *in, char *out, size_t cap) {
    size_t o = 0; bool after_key = false;
    if (!cap) return 0;
    for (size_t i = 0; in[i] && o + 12 < cap;) {
        if (!lognet_wc((unsigned char)in[i])) { if (in[i] == '\n') after_key = false; out[o++] = in[i++]; continue; }
        size_t j = i; while (in[j] && lognet_wc((unsigned char)in[j])) j++;
        size_t n = j - i; const char *w = in + i;
        /* chave=valor ou chave:valor dentro da própria palavra: cobre só o valor */
        size_t sep = 0; for (size_t k = 0; k < n; k++) if (w[k] == '=' || w[k] == ':') { sep = k + 1; break; }
        bool kv = sep > 0 && sep < n + 1 && lognet_keyword(w, sep - 1);
        if (kv) { memcpy(out + o, w, sep); o += sep; if (sep < n) { memcpy(out + o, "[oculto]", 8); o += 8; } else after_key = true; }
        else if (after_key && n > 0) { memcpy(out + o, "[oculto]", 8); o += 8; after_key = false; }
        else {
            int c = lognet_class(w, n);
            const char *r = c == 1 ? "[oculto]" : c == 2 ? "[ip]" : c == 3 ? "[email]" : NULL;
            if (r) { size_t L = strlen(r); memcpy(out + o, r, L); o += L; }
            else { if (o + n + 1 >= cap) n = cap - o - 1; memcpy(out + o, w, n); o += n; }
            if (!r && lognet_keyword(w, n)) after_key = true;   /* "token abc": a próxima palavra é o segredo */
        }
        i = j;
    }
    out[o] = 0; return o;
}
/* fica só com as últimas `max_lines` linhas de `text` (modifica nada; devolve ponteiro dentro de `text`) */
static inline const char *lognet_tail(const char *text, int max_lines) {
    size_t n = strlen(text); int lines = 0; const char *p = text + n;
    if (n && text[n - 1] == '\n') { p--; }
    while (p > text) { p--; if (*p == '\n' && ++lines >= max_lines) return p + 1; }
    return text;
}
#endif

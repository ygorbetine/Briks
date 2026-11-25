/* Regras puras do online/offline entre amigos (ver ppres.h); sem janela, testáveis à parte. */
#ifndef BRIKS_PPRES_MODEL_H
#define BRIKS_PPRES_MODEL_H
#include <stdbool.h>
#define PPRES_REFRESH_MS 120000ULL   /* de quanto em quanto tempo me anuncio de novo */
#define PPRES_TTL_MS     270000ULL   /* sem aviso por tanto tempo = offline */
#define PPRES_REPLY_MIN_MS 20000ULL  /* no máximo uma resposta por amigo a cada 20 s */
/* last = 0 significa nunca visto (ou saiu: "bye") */
static inline bool ppres_fresh(unsigned long long last, unsigned long long now) { return last && now >= last && now - last < PPRES_TTL_MS; }
static inline bool ppres_may_reply(unsigned long long last_reply, unsigned long long now) { return !last_reply || now < last_reply || now - last_reply >= PPRES_REPLY_MIN_MS; }
#endif

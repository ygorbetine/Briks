/* Teste do perfil local (profile.c) sem janela: prazo do status, expiração, lista "Ignorar programa" e persistência. */
#include "../src/profile.c"
briks_theme_t g_th; int g_perf;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALHOU: %s (linha %d)\n", #c, __LINE__); fails++; } } while (0)
int main(void) {
    static app_state_t s; memset(&s, 0, sizeof s);
    long long now = 1800000000LL;
    CHECK(profile_expiry_for(0, now) == 0 && profile_expiry_for(1, now) == now + 3600 && profile_expiry_for(2, now) == now + 4 * 3600);
    long long today = profile_expiry_for(3, now);
    CHECK(today > now && today <= now + 86400 + 3600);
    /* texto de usuário: sem controles, quebras, bidi, UTF-8 inválido; espaços colapsados; limite em caracteres */
    { char t[128];
      snprintf(t, sizeof t, "  Jogando\r\n  Valorant\t\x01\x7f  "); profile_clean_text(t, 64); CHECK(!strcmp(t, "Jogando Valorant"));
      snprintf(t, sizeof t, "a\xE2\x80\xAE" "b\xE2\x80\x8B" "c"); profile_clean_text(t, 64); CHECK(!strcmp(t, "abc"));   /* bidi override e zero-width */
      snprintf(t, sizeof t, "ol\xC3\xA1 \xFF\xC3"); profile_clean_text(t, 64); CHECK(!strcmp(t, "ol\xC3\xA1"));            /* byte solto e UTF-8 truncado */
      snprintf(t, sizeof t, "\"quoted\" \\ ok"); CHECK(profile_clean_text(t, 64) == 13 && !strcmp(t, "\"quoted\" \\ ok"));   /* aspas/barra ficam (o JSON escapa) */
      memset(t, 'x', 100); t[100] = 0; CHECK(profile_clean_text(t, 64) == 64 && strlen(t) == 64);
      snprintf(t, sizeof t, "\xC3\xA1\xC3\xA1\xC3\xA1"); CHECK(profile_clean_text(t, 2) == 2 && !strcmp(t, "\xC3\xA1\xC3\xA1"));   /* corta por caractere, não por byte */
      snprintf(t, sizeof t, " \n\t "); CHECK(profile_clean_text(t, 64) == 0 && !t[0]);
      char e[200]; snprintf(t, sizeof t, "a\"b\\c"); json_escape(t, e, sizeof e); CHECK(!strcmp(e, "a\\\"b\\\\c")); }
    /* ignorar */
    snprintf(s.rp_ignore[0], sizeof s.rp_ignore[0], "Spotify"); s.rp_ignore_n = 1;
    CHECK(profile_ignored(&s, "spotify", NULL) && profile_ignored(&s, "SPOTIFY", "x") && !profile_ignored(&s, "chrome", "Usando Google Chrome"));
    CHECK(profile_ignored(&s, "x", "Ouvindo no spotify"));
    /* persistência e expiração, em arquivo de teste */
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s/briks_profile_test.json", getenv("TEMP") ? getenv("TEMP") : "."); remove(path);
    _putenv_s("BRIKS_PROFILE_FILE", path);
    memset(&s, 0, sizeof s);
    profile_tick(&s);                                      /* 1º quadro: só carrega */
    snprintf(s.user_status, sizeof s.user_status, "Reunião"); snprintf(s.user_emoji, sizeof s.user_emoji, "X"); s.status_clear = 1; s.invisible = true;
    snprintf(s.rp_ignore[0], sizeof s.rp_ignore[0], "discord"); s.rp_ignore_n = 1;
    profile_tick(&s);                                      /* mudou: grava e calcula o prazo */
    long long t = (long long)time(NULL);
    CHECK(s.status_expires >= t + 3590 && s.status_expires <= t + 3610);
    FILE *f = fopen(path, "rb"); char buf[1024] = ""; if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    CHECK(strstr(buf, "\"status\":\"Reunião\"") && strstr(buf, "\"clear\":1") && strstr(buf, "\"invisible\":true") && strstr(buf, "\"ignore\":[\"discord\"]") && strstr(buf, "\"emoji\":\"X\""));
    if (fails) printf("arquivo: %s\n", buf);
    s.status_expires = t - 5;                              /* venceu */
    profile_tick(&s);
    CHECK(!s.user_status[0] && !s.user_emoji[0] && s.status_expires == 0);
    printf(fails ? "profile_test: %d falha(s)\n" : "profile_test: ok\n", fails);
    remove(path);
    return fails ? 1 : 0;
}

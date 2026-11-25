#include "../l8w8jwt/include/l8w8jwt/encode.h"
#include "../l8w8jwt/include/l8w8jwt/decode.h"
#include "worker.h"
#include <myhead.h>

static unsigned char tokenKey[64];
static int tokenKeyReady;
// JWT 库的 C 接口使用 char*。
static char issuer[] = "net-disk";
static char audience[] = "net-disk-transfer";

static int read_token_bytes(int fd, unsigned char *buf, size_t length)
{
    size_t total = 0;
    while(total < length)
    {
        ssize_t ret = read(fd, buf + total, length - total);
        if(ret == -1 && errno == EINTR)
        {
            continue;
        }
        if(ret <= 0)
        {
            return -1;
        }
        total += ret;
    }
    return 0;
}

int init_token_key()
{
    int fd = open("./config/token.key", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(fd == -1)
    {
        return -1;
    }
    struct stat statbuf;
    int ret = -1;
    if(fstat(fd, &statbuf) == 0 && S_ISREG(statbuf.st_mode) &&
       statbuf.st_uid == geteuid() && (statbuf.st_mode & 077) == 0 &&
       statbuf.st_size == (off_t)sizeof(tokenKey))
    {
        ret = read_token_bytes(fd, tokenKey, sizeof(tokenKey));
    }
    close(fd);
    if(ret != 0)
    {
        memset(tokenKey, 0, sizeof(tokenKey));
        tokenKeyReady = 0;
        return -1;
    }
    tokenKeyReady = 1;
    return 0;
}

int make_session_id(char *sid)
{
    unsigned char random[TOKEN_SESSION_LENGTH / 2];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if(fd == -1)
    {
        return -1;
    }
    int ret = read_token_bytes(fd, random, sizeof(random));
    close(fd);
    if(ret != 0)
    {
        return -1;
    }
    static const char hex[] = "0123456789abcdef";
    for(size_t i = 0; i < sizeof(random); ++i)
    {
        sid[i * 2] = hex[random[i] >> 4];
        sid[i * 2 + 1] = hex[random[i] & 15];
    }
    sid[TOKEN_SESSION_LENGTH] = '\0';
    return 0;
}

int encode(connection_t *pConnection, char *token)
{
    if(!tokenKeyReady || strlen(pConnection->sid) != TOKEN_SESSION_LENGTH)
    {
        return -1;
    }
    char controlFd[32];
    int length = snprintf(controlFd, sizeof(controlFd), "%d", pConnection->netFd);
    if(length <= 0 || length >= (int)sizeof(controlFd))
    {
        return -1;
    }
    char sessionKey[] = "sid";
    char controlKey[] = "control_fd";
    struct l8w8jwt_claim claims[2] = {
        {sessionKey, 3, pConnection->sid, TOKEN_SESSION_LENGTH, L8W8JWT_CLAIM_TYPE_STRING},
        {controlKey, 10, controlFd, (size_t)length, L8W8JWT_CLAIM_TYPE_INTEGER}
    };
    char *jwt = NULL;
    size_t jwt_length = 0;
    struct l8w8jwt_encoding_params params;
    l8w8jwt_encoding_params_init(&params);
    params.alg = L8W8JWT_ALG_HS512;
    params.sub = pConnection->user_name;
    params.iss = issuer;
    params.aud = audience;
    params.iat = time(NULL);
    if(params.iat <= 0)
    {
        return -1;
    }
    params.exp = params.iat + TOKEN_LIFETIME;
    params.additional_payload_claims = claims;
    params.additional_payload_claims_count = 2;
    params.secret_key = tokenKey;
    params.secret_key_length = sizeof(tokenKey);
    params.out = &jwt;
    params.out_length = &jwt_length;
    int ret = l8w8jwt_encode(&params);
    if(ret != L8W8JWT_SUCCESS || jwt == NULL ||
       strnlen(jwt, PROTOCOL_TOKEN_CAPACITY) >= PROTOCOL_TOKEN_CAPACITY)
    {
        l8w8jwt_free(jwt);
        return -1;
    }
    strcpy(token, jwt);
    l8w8jwt_free(jwt);
    return 0;
}

// 必要字段只能出现一次，防止同名字段产生不同解释。
static struct l8w8jwt_claim *find_token_claim(struct l8w8jwt_claim *claims,
                                            size_t count, const char *key, int type)
{
    struct l8w8jwt_claim *found = NULL;
    size_t length = strlen(key);
    for(size_t i = 0; i < count; ++i)
    {
        if(claims[i].key_length == length && memcmp(claims[i].key, key, length) == 0)
        {
            if(found != NULL || claims[i].type != type || claims[i].value == NULL ||
               strlen(claims[i].value) != claims[i].value_length)
            {
                return NULL;
            }
            found = &claims[i];
        }
    }
    return found;
}

static int token_number(struct l8w8jwt_claim *claim, int64_t *value)
{
    if(claim == NULL || claim->value_length == 0 ||
       strspn(claim->value, "0123456789") != claim->value_length)
    {
        return -1;
    }
    char *end;
    errno = 0;
    long long number = strtoll(claim->value, &end, 10);
    if(errno != 0 || *end != '\0' || number < 0)
    {
        return -1;
    }
    *value = number;
    return 0;
}

int decode(char *user_name, char *token, clientQueue_t *pclientQueue, int *puser_id)
{
    *puser_id = -1;
    if(!tokenKeyReady)
    {
        return TOKEN_INVALID;
    }
    struct l8w8jwt_decoding_params params;
    l8w8jwt_decoding_params_init(&params);
    params.alg = L8W8JWT_ALG_HS512;
    params.jwt = token;
    params.jwt_length = strlen(token);
    params.verification_key = tokenKey;
    params.verification_key_length = sizeof(tokenKey);
    params.validate_iss = issuer;
    params.validate_sub = user_name;
    params.validate_aud = audience;
    params.validate_exp = 1;
    params.exp_tolerance_seconds = 0;
    params.validate_iat = 1;
    params.iat_tolerance_seconds = 60;
    struct l8w8jwt_claim *claims = NULL;
    size_t count = 0;
    enum l8w8jwt_validation_result validation = L8W8JWT_VALID;
    int decoded = l8w8jwt_decode(&params, &validation, &claims, &count);
    struct ClaimList {
        l8w8jwt_claim *claims;
        size_t count;
        ~ClaimList() { l8w8jwt_free_claims(claims, count); }
    } owner{claims, count};
    if(decoded != L8W8JWT_SUCCESS || claims == NULL ||
       (validation != L8W8JWT_VALID && validation != L8W8JWT_EXP_FAILURE))
    {
        return TOKEN_INVALID;
    }
    struct l8w8jwt_claim *alg = find_token_claim(claims, count, "alg", L8W8JWT_CLAIM_TYPE_STRING);
    struct l8w8jwt_claim *sub = find_token_claim(claims, count, "sub", L8W8JWT_CLAIM_TYPE_STRING);
    struct l8w8jwt_claim *iss = find_token_claim(claims, count, "iss", L8W8JWT_CLAIM_TYPE_STRING);
    struct l8w8jwt_claim *aud = find_token_claim(claims, count, "aud", L8W8JWT_CLAIM_TYPE_STRING);
    struct l8w8jwt_claim *sid = find_token_claim(claims, count, "sid", L8W8JWT_CLAIM_TYPE_STRING);
    int64_t controlFd, issuedAt, expiresAt;
    if(alg == NULL || strcmp(alg->value, "HS512") != 0 ||
       sub == NULL || strcmp(sub->value, user_name) != 0 ||
       iss == NULL || strcmp(iss->value, "net-disk") != 0 ||
       aud == NULL || strcmp(aud->value, "net-disk-transfer") != 0 ||
       sid == NULL || sid->value_length != TOKEN_SESSION_LENGTH ||
       token_number(find_token_claim(claims, count, "control_fd", L8W8JWT_CLAIM_TYPE_INTEGER), &controlFd) != 0 ||
       token_number(find_token_claim(claims, count, "iat", L8W8JWT_CLAIM_TYPE_INTEGER), &issuedAt) != 0 ||
       token_number(find_token_claim(claims, count, "exp", L8W8JWT_CLAIM_TYPE_INTEGER), &expiresAt) != 0 ||
       issuedAt <= 0 || expiresAt <= issuedAt || expiresAt - issuedAt != TOKEN_LIFETIME ||
       controlFd > INT_MAX || (uint64_t)controlFd >= pclientQueue->fd_capacity)
    {
        return TOKEN_INVALID;
    }
    // fd 只用于定位，必须同时匹配仍登录的控制连接及随机会话标识。
    connection_t *pControl = pclientQueue->connections[controlFd].get();
    if(pControl == NULL || pclientQueue->client[controlFd] < 0 ||
       strcmp(pControl->user_name, user_name) != 0 ||
       strcmp(pControl->sid, sid->value) != 0)
    {
        return TOKEN_INVALID;
    }
    time_t now = time(NULL);
    if(now == (time_t)-1)
    {
        return TOKEN_INVALID;
    }
    if(validation == L8W8JWT_EXP_FAILURE || (int64_t)now >= expiresAt)
    {
        return TOKEN_EXPIRED;
    }
    *puser_id = pclientQueue->client[controlFd];
    return TOKEN_VALID;
}

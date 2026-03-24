#ifndef USERS_H
#define USERS_H

#include "common.h"

#define USER_ROLE_USER 0
#define USER_ROLE_ADMIN 1

#define USER_FLAG_CAN_SUDO 0x01

#define USERNAME_MAX_LEN 16
#define PASSWORD_INPUT_MAX_LEN 16
#define PASSWORD_HASH_LEN 9
#define PASSWORD_SALT_LEN 9
#define USER_HOME_MAX_LEN 64
#define MAX_USERS 16

typedef struct user_account {
    char username[USERNAME_MAX_LEN];
    char password_hash[PASSWORD_HASH_LEN];
    char password_salt[PASSWORD_SALT_LEN];
    char home[USER_HOME_MAX_LEN];
    uint16_t uid;
    uint16_t gid;
    uint8_t role;
    uint8_t flags;
    uint8_t active;
} user_account_t;

void users_init(void);
int users_load_from_disk(void);
int users_login(const char *username, const char *password);
int users_su(const char *username, const char *password);
int users_try_autologin(void);
int users_begin_sudo(const char *password);
void users_end_sudo(void);
int users_add(const char *username, const char *password, uint8_t role, int can_sudo);
int users_remove(const char *username);
int users_set_password(const char *username, const char *password);
int users_setup_install_owner(const char *username, const char *user_password, const char *root_password, int enable_autologin);
const user_account_t *users_find(const char *username);
const user_account_t *users_find_by_uid(uint16_t uid);
const user_account_t *users_current(void);
const user_account_t *users_effective(void);
int users_autologin_enabled(void);
const char *users_autologin_username(void);
void users_set_autologin(int enabled, const char *username);
int users_system_is_installed(void);
int users_is_root(void);
int users_effective_is_root(void);
int users_can_manage_accounts(void);
int users_current_can_sudo(void);
void users_list(void);
void users_print_id(const char *username);
void users_print_groups(const char *username);
void users_persist(void);
void users_bootstrap_fs(void);

#endif

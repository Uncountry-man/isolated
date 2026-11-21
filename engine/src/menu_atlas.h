// GERADO por tools/make_menu.py - nao editar. Retangulos (pixels) dos sprites em assets/menu.e0i.
#pragma once
namespace e0 {
static constexpr int kMenuAtlasW = 2048, kMenuAtlasH = 1856;
struct MenuSprite { int x, y, w, h; };
enum MenuSpriteId {
    MS_TITLE,
    MS_SUBTITLE,
    MS_ORNAMENT,
    MS_PLAY,
    MS_CHARACTER,
    MS_SETTINGS,
    MS_QUIT,
    MS_SET_HEADER,
    MS_SET_QUALITY,
    MS_SET_FILTER,
    MS_SET_SOUND,
    MS_SET_BACK,
    MS_V_AUTO,
    MS_V_LOW,
    MS_V_MED,
    MS_V_HIGH,
    MS_V_OFF,
    MS_V_SOFT,
    MS_V_STRONG,
    MS_V_ON,
    MS_SOON,
    MS_INVITE,
    MS_CREDITS,
    MS_CRED_HEADER,
    MS_SET_SERVER,
    MS_V_DEFAULT,
    MS_SRV_ONLINE,
    MS_SRV_OFFLINE,
    MS_SRV_CHECK,
    MS_SRV_LOCAL,
    MS_BAD_ADDR,
    MS_MS,
    MS_D0,
    MS_D1,
    MS_D2,
    MS_D3,
    MS_D4,
    MS_D5,
    MS_D6,
    MS_D7,
    MS_D8,
    MS_D9,
    MS_DDOT,
    MS_DCOLON,
    MS_CRED_H0,
    MS_CRED_B0,
    MS_CRED_H1,
    MS_CRED_B1,
    MS_CRED_H2,
    MS_CRED_B2,
    MS_CRED_H3,
    MS_CRED_B3,
    MS_CRED_H4,
    MS_CRED_B4,
    MS_VERSION,
    MS_STAR,
    MS_LOGIN_HEAD,
    MS_LOGIN_GOOGLE,
    MS_LOGIN_GUEST,
    MS_LOGIN_NOTE,
    MS_LOGIN_BUSY,
    MS_LOGIN_ERR_CFG,
    MS_LOGIN_ERR,
    MS_LOGIN_CANCEL,
    MS_LOGIN_NET,
    MS_ACC_GUEST,
    MS_ACC_GOOGLE,
    MS_SET_ACCOUNT,
    MS_V_LOGOUT,
    MS_G_LOGO,
    MS_ICON_DISCORD,
    MS_ICON_YOUTUBE,
    MS_ICON_INSTAGRAM,
    MS_ICON_TIKTOK,
    MS_CREW,
    MS_LOGIN_EMAIL,
    MS_ACC_EMAIL,
    MS_LOGIN_ERR_CRED,
    MS_LOGIN_ERR_EMAIL,
    MS_LOGIN_ERR_MANY,
    MS_LOGIN_ERR_EMPTY,
    MS_COUNT
};
static constexpr MenuSprite kMenuSprites[MS_COUNT] = {
    {0, 0, 995, 339},  // TITLE
    {0, 1071, 782, 67},  // SUBTITLE
    {0, 1810, 760, 40},  // ORNAMENT
    {1647, 0, 321, 119},  // PLAY
    {0, 341, 668, 119},  // CHARACTER
    {670, 341, 848, 119},  // SETTINGS
    {1520, 341, 225, 119},  // QUIT
    {631, 583, 804, 110},  // SET_HEADER
    {0, 704, 432, 91},  // SET_QUALITY
    {434, 704, 550, 91},  // SET_FILTER
    {986, 704, 163, 91},  // SET_SOUND
    {0, 462, 391, 119},  // SET_BACK
    {1151, 704, 208, 91},  // V_AUTO
    {1361, 704, 223, 91},  // V_LOW
    {1586, 704, 240, 91},  // V_MED
    {1828, 704, 187, 91},  // V_HIGH
    {0, 797, 425, 91},  // V_OFF
    {427, 797, 237, 91},  // V_SOFT
    {666, 797, 240, 91},  // V_STRONG
    {908, 797, 289, 91},  // V_ON
    {1517, 983, 309, 75},  // SOON
    {393, 462, 945, 119},  // INVITE
    {1340, 462, 496, 119},  // CREDITS
    {1437, 583, 473, 110},  // CRED_HEADER
    {1199, 797, 374, 91},  // SET_SERVER
    {1575, 797, 305, 91},  // V_DEFAULT
    {784, 1071, 206, 67},  // SRV_ONLINE
    {992, 1071, 224, 67},  // SRV_OFFLINE
    {1218, 1071, 363, 67},  // SRV_CHECK
    {1583, 1071, 352, 67},  // SRV_LOCAL
    {0, 1140, 558, 67},  // BAD_ADDR
    {560, 1140, 78, 67},  // MS
    {804, 1209, 32, 61},  // D0
    {838, 1209, 21, 61},  // D1
    {861, 1209, 30, 61},  // D2
    {893, 1209, 28, 61},  // D3
    {923, 1209, 30, 61},  // D4
    {955, 1209, 28, 61},  // D5
    {985, 1209, 31, 61},  // D6
    {1018, 1209, 27, 61},  // D7
    {1047, 1209, 30, 61},  // D8
    {1079, 1209, 31, 61},  // D9
    {1112, 1209, 14, 61},  // DDOT
    {1128, 1209, 14, 61},  // DCOLON
    {1754, 1696, 122, 54},  // CRED_H0
    {1144, 1209, 877, 59},  // CRED_B0
    {0, 1754, 634, 54},  // CRED_H1
    {0, 1278, 1202, 59},  // CRED_B1
    {636, 1754, 382, 54},  // CRED_H2
    {0, 1339, 1399, 59},  // CRED_B2
    {1020, 1754, 173, 54},  // CRED_H3
    {0, 1400, 896, 59},  // CRED_B3
    {1195, 1754, 363, 54},  // CRED_H4
    {0, 1461, 1479, 59},  // CRED_B4
    {1481, 1461, 87, 59},  // VERSION
    {1828, 983, 72, 72},  // STAR
    {640, 1140, 993, 67},  // LOGIN_HEAD
    {836, 890, 748, 86},  // LOGIN_GOOGLE
    {0, 983, 818, 86},  // LOGIN_GUEST
    {0, 1522, 1059, 56},  // LOGIN_NOTE
    {1061, 1522, 329, 56},  // LOGIN_BUSY
    {0, 1580, 922, 56},  // LOGIN_ERR_CFG
    {924, 1580, 904, 56},  // LOGIN_ERR
    {0, 1638, 403, 56},  // LOGIN_CANCEL
    {405, 1638, 793, 56},  // LOGIN_NET
    {1635, 1140, 277, 67},  // ACC_GUEST
    {0, 1209, 420, 67},  // ACC_GOOGLE
    {0, 890, 262, 91},  // SET_ACCOUNT
    {264, 890, 570, 91},  // V_LOGOUT
    {997, 0, 128, 128},  // G_LOGO
    {1127, 0, 128, 128},  // ICON_DISCORD
    {1257, 0, 128, 128},  // ICON_YOUTUBE
    {1387, 0, 128, 128},  // ICON_INSTAGRAM
    {1517, 0, 128, 128},  // ICON_TIKTOK
    {0, 583, 629, 119},  // CREW
    {820, 983, 695, 86},  // LOGIN_EMAIL
    {422, 1209, 380, 67},  // ACC_EMAIL
    {1200, 1638, 643, 56},  // LOGIN_ERR_CRED
    {0, 1696, 362, 56},  // LOGIN_ERR_EMAIL
    {364, 1696, 821, 56},  // LOGIN_ERR_MANY
    {1187, 1696, 565, 56},  // LOGIN_ERR_EMPTY
};
}  // namespace e0

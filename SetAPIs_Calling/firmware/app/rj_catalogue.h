#ifndef RJ_CATALOGUE_H
#define RJ_CATALOGUE_H
typedef struct { const char *id, *metadata, *schema, *roles; } RjProfile;
extern const RjProfile rj_profiles[];
extern const unsigned rj_profile_count;
#endif

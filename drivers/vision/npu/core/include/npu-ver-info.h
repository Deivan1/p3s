#ifndef _NPU_VER_INFO_H
#define _NPU_VER_INFO_H

#define	DD_VERSION_MAJOR	"1"
#define	DD_VERSION_MIDDLE	"1"
#define	DD_VERSION_MINOR	"I26f62cb9b"

extern const char *npu_git_log_str;
extern const char *npu_git_hash_str;
extern const char *npu_build_info;

#define DD_VER_STR	DD_VERSION_MAJOR "." DD_VERSION_MIDDLE "." DD_VERSION_MINOR " "
#endif

/* Code to mangle pathnames into those matching a given prefix.
   eg. open("/lib/foo.so") => open("/usr/gnemul/i386-linux/lib/foo.so");

   The assumption is that this area does not change.
*/
#include "qemu/osdep.h"
#include <sys/param.h>
#include <dirent.h>
#include "qemu/cutils.h"

struct pathelem
{
    /* Name of this, eg. lib */
    char *name;
    /* Full path name, eg. /usr/gnemul/x86-linux/lib. */
    char *pathname;
    struct pathelem *parent;
    /* Children */
    unsigned int num_entries;
    struct pathelem *entries[0];
};

static struct pathelem *add_entry(struct pathelem *root, const char *name,
                                  unsigned type);

static struct pathelem *new_entry(const char *root,
                                  struct pathelem *parent,
                                  const char *name)
{
    struct pathelem *new = g_malloc(sizeof(*new));
    new->name = g_strdup(name);
    new->pathname = g_strdup_printf("%s/%s", root, name);
    new->num_entries = 0;
    return new;
}

#define streq(a,b) (strcmp((a), (b)) == 0)

/* Not all systems provide this feature */
#if defined(DT_DIR) && defined(DT_UNKNOWN) && defined(DT_LNK)
# define dirent_type(dirent) ((dirent)->d_type)
# define is_dir_maybe(type) \
    ((type) == DT_DIR || (type) == DT_UNKNOWN || (type) == DT_LNK)
#else
# define dirent_type(dirent) (1)
# define is_dir_maybe(type)  (type)
#endif

static struct pathelem *add_dir_maybe(struct pathelem *path)
{
    DIR *dir;

    if ((dir = opendir(path->pathname)) != NULL) {
        struct dirent *dirent;

        while ((dirent = readdir(dir)) != NULL) {
            if (!streq(dirent->d_name,".") && !streq(dirent->d_name,"..")){
                path = add_entry(path, dirent->d_name, dirent_type(dirent));
            }
        }
        closedir(dir);
    }
    return path;
}

static struct pathelem *add_entry(struct pathelem *root, const char *name,
                                  unsigned type)
{
    struct pathelem **e;

    root->num_entries++;

    root = g_realloc(root, sizeof(*root)
                   + sizeof(root->entries[0])*root->num_entries);
    e = &root->entries[root->num_entries-1];

    *e = new_entry(root->pathname, root, name);
    if (is_dir_maybe(type)) {
        *e = add_dir_maybe(*e);
    }

    return root;
}


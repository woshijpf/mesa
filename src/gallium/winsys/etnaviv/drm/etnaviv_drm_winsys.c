#include <sys/stat.h>

#include "util/u_hash_table.h"
#include "util/u_memory.h"

/*
 * Copyright (c) 2015 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include "etnaviv/etnaviv_screen.h"
#include "etnaviv_drm_public.h"

#include <fcntl.h>
#include <stdio.h>

static struct pipe_screen *
etna_drm_screen_create_fd(int fd, struct renderonly *ro)
{
   struct etna_device *dev;
   struct etna_gpu *gpu;
   uint64_t val;
   int i;

   dev = etna_device_new(fd);
   if (!dev) {
      fprintf(stderr, "Error creating device\n");
      return NULL;
   }

   for (i = 0;; i++) {
      gpu = etna_gpu_new(dev, i);
      if (!gpu) {
         fprintf(stderr, "Error creating gpu\n");
         return NULL;
      }

      /* Look for a 3D capable GPU */
      if (etna_gpu_get_param(gpu, ETNA_GPU_FEATURES_0, &val) == 0 &&
          val & (1 << 2))
         break;

      etna_gpu_del(gpu);
   }

   return etna_screen_create(dev, gpu, ro);
}

struct pipe_screen *
etna_drm_screen_create_native(struct renderonly *ro)
{
   struct pipe_screen *screen;
   int fd = ro->fd;

   screen = etna_drm_screen_create_fd(fd, ro);
   if (!screen)
      return NULL;

   return screen;
}

struct pipe_screen *
etna_drm_screen_create_rendernode(struct renderonly *ro)
{
   struct pipe_screen *screen;
   int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);

   if (fd == -1)
      return NULL;

   screen = etna_drm_screen_create_fd(fd, ro);
   if (!screen) {
      close(fd);
      return NULL;
   }

   return screen;
}

static const struct renderonly_ops etna_native_ro_ops = {
   .intermediate_renderong = true,
   .create = etna_drm_screen_create_native
};

static struct util_hash_table *fd_tab = NULL;

pipe_static_mutex(fd_screen_mutex);

static void
etna_drm_screen_destroy(struct pipe_screen *pscreen)
{
    struct etna_screen *screen = etna_screen(pscreen);
    boolean destroy;

    pipe_mutex_lock(fd_screen_mutex);
    destroy = --screen->refcnt == 0;
    if (destroy) {
        int fd = screen->ro->fd;
        util_hash_table_remove(fd_tab, intptr_to_pointer(fd));
    }
    pipe_mutex_unlock(fd_screen_mutex);

    if (destroy) {
        pscreen->destroy = screen->winsys_priv;
        pscreen->destroy(pscreen);
    }
}

static unsigned hash_fd(void *key)
{
    int fd = pointer_to_intptr(key);
    struct stat stat;
    fstat(fd, &stat);

    return stat.st_dev ^ stat.st_ino ^ stat.st_rdev;
}

static int compare_fd(void *key1, void *key2)
{
    int fd1 = pointer_to_intptr(key1);
    int fd2 = pointer_to_intptr(key2);
    struct stat stat1, stat2;
    fstat(fd1, &stat1);
    fstat(fd2, &stat2);

    return stat1.st_dev != stat2.st_dev ||
            stat1.st_ino != stat2.st_ino ||
            stat1.st_rdev != stat2.st_rdev;
}

struct pipe_screen *
etna_drm_screen_create(int fd)
{
    struct pipe_screen *pscreen = NULL;

    pipe_mutex_lock(fd_screen_mutex);
    if (!fd_tab) {
        fd_tab = util_hash_table_create(hash_fd, compare_fd);
        if (!fd_tab)
            goto unlock;
    }

    pscreen = util_hash_table_get(fd_tab, intptr_to_pointer(fd));
    if (pscreen) {
        etna_screen(pscreen)->refcnt++;
    } else {
        int dupfd = dup(fd);
        pscreen = renderonly_screen_create(dupfd, &etna_native_ro_ops, NULL);
        if (pscreen) {
            util_hash_table_set(fd_tab, intptr_to_pointer(dupfd), pscreen);

            /* Bit of a hack, to avoid circular linkage dependency,
             * ie. pipe driver having to call in to winsys, we
             * override the pipe drivers screen->destroy():
             */
            etna_screen(pscreen)->winsys_priv = pscreen->destroy;
            pscreen->destroy = etna_drm_screen_destroy;
        } else {
            close(dupfd);
        }
    }

unlock:
    pipe_mutex_unlock(fd_screen_mutex);
    return pscreen;
}

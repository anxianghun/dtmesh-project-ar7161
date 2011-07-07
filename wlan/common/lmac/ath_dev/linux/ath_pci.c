/*
 * Copyright (c) 2009, Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 * 
 */

#include <osdep.h>

/* return bus cachesize in 4B word units */
void
bus_read_cachesize(osdev_t osdev, int *csz)
{
    u_int8_t  u8tmp;

    pci_read_config_byte((struct pci_dev *)osdev->bdev, PCI_CACHE_LINE_SIZE, (u_int8_t *)&u8tmp);
    *csz = (int) u8tmp;
    /*
    ** This check was put in to avoid "unplesant" consequences if the bootrom
    ** has not fully initialized all PCI devices.  Sometimes the cache line size
    ** register is not set
    */
    
    if(*csz == 0){
        *csz = DEFAULT_CACHELINE >> 2;   // Use the default size
    }
}


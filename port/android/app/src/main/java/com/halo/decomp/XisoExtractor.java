package com.halo.decomp;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Copies the maps folder out of an Xbox disc image (an "xiso", or a whole
 * disc's ".iso"), as the desktop games do (port/linux/src/xiso.c, which this
 * follows).
 *
 * The image's file system is XDVDFS, read as extract-xiso does
 * (https://github.com/XboxDev/extract-xiso, extract-xiso.c, whose format
 * handling this follows; its license is below and in
 * port/third_party/extract-xiso/LICENSE.TXT): 2048-byte sectors; a volume
 * descriptor at 0x10000 that starts and ends with "MICROSOFT*XBOX*MEDIA" and
 * gives the root directory's sector and size; and directories whose entries
 * form a binary tree (each entry: left and right subtree offsets in 4-byte
 * units, the start sector, the size, attributes, the name's length and the
 * name, 4-byte aligned). Images made from a whole disc put the game partition
 * further in, at one of the offsets below.
 *
 * The files are written to maps.partial first and moved into maps once they
 * all are, so an interrupted extraction never leaves maps/ui.map behind.
 *
 * Some parts of this code are copyright in@fishtank.com. This product
 * includes software developed by in &lt;in@fishtank.com&gt;.
 *
 * Copyright (c) 2003 in &lt;in@fishtank.com&gt;
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *    This product includes software developed by in &lt;in@fishtank.com&gt;.
 *
 * 4. Neither the name of "in" nor the email address "in@fishtank.com"
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED `AS IS' AND ANY EXPRESS OR IMPLIED WARRANTIES
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * AUTHOR OR ANY CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
final class XisoExtractor {
    private static final int SECTOR_SIZE = 2048;
    private static final long VOLUME_DESCRIPTOR_OFFSET = 0x10000;
    private static final int ENTRY_HEADER_SIZE = 14;
    private static final int ATTRIBUTE_DIRECTORY = 0x10;
    /* directory tables are a few sectors; anything much larger is not one */
    private static final long MAXIMUM_DIRECTORY_SIZE = 4 << 20;
    private static final int MAXIMUM_FILES = 256;
    private static final int COPY_BUFFER_SIZE = 1 << 20;
    private static final byte[] VOLUME_MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);

    /* where the game partition starts: a plain image, and whole-disc images
    (extract-xiso's GLOBAL, XGD3 and XGD1 offsets) */
    private static final long[] PARTITION_OFFSETS = { 0, 0x0FD90000L, 0x02080000L, 0x18300000L };

    interface Progress {
        /** called as the copy goes; returns false to stop */
        boolean report(String file, long done, long total);
    }

    /** a reason the player can act on */
    static final class ExtractException extends IOException {
        ExtractException(String message) {
            super(message);
        }
    }

    /** what probing an image found: whether it is a Halo disc and how whole it is */
    static final class Info {
        boolean halo;
        int mapCount;
        long dataBytes;
        long fileBytes;
        boolean complete;

        String summary() {
            if (!halo)
                return "NOT A HALO DISC  ·  " + human(fileBytes);
            return mapCount + " MAPS  ·  " + human(dataBytes) + (complete ? "  ·  READY" : "  ·  INCOMPLETE");
        }

        private static String human(long bytes) {
            if (bytes >= 1L << 30)
                return String.format(java.util.Locale.US, "%.1f GB", bytes / (double) (1L << 30));
            return String.format(java.util.Locale.US, "%.0f MB", bytes / (double) (1L << 20));
        }
    }

    private static final class Entry {
        final String name;
        final long sector;
        final long size;

        Entry(String name, long sector, long size) {
            this.name = name;
            this.sector = sector;
            this.size = size;
        }
    }

    private final FileChannel image;
    private long partition;

    private XisoExtractor(FileChannel image) {
        this.image = image;
    }

    /** copies the image's maps folder to destination/maps */
    static void extractMaps(FileChannel image, File destination, Progress progress) throws IOException {
        new XisoExtractor(image).extract(destination, progress);
    }

    /**
     * Reads an image far enough to tell a Halo disc from another ROM, and
     * whether every map it names lies inside the file.
     */
    static Info probe(FileChannel image) {
        Info info = new Info();
        try {
            info.fileBytes = image.size();
            XisoExtractor extractor = new XisoExtractor(image);
            long[] root = extractor.findVolume();
            ByteBuffer table = extractor.readDirectory(root[0], root[1], "The disc image's file system is damaged.");
            List<Entry> directories = new ArrayList<>();
            walk(table, 0, 0, true, directories, new int[1]);
            Entry maps = find(directories, "maps");
            if (maps == null)
                return info;
            table = extractor.readDirectory(maps.sector, maps.size, "The disc image's maps folder is damaged.");
            List<Entry> files = new ArrayList<>();
            walk(table, 0, 0, false, files, new int[1]);
            if (find(files, "ui.map") == null)
                return info;
            info.halo = true;
            info.mapCount = files.size();
            long length = image.size();
            long total = 0;
            boolean complete = true;
            for (Entry file : files) {
                total += file.size;
                if (extractor.partition + file.sector * SECTOR_SIZE + file.size > length)
                    complete = false;
            }
            info.dataBytes = total;
            info.complete = complete;
        } catch (IOException | RuntimeException exception) {
            // not readable as an Xbox disc
        }
        return info;
    }

    private static Entry find(List<Entry> entries, String name) {
        for (Entry entry : entries) {
            if (entry.name.equalsIgnoreCase(name))
                return entry;
        }
        return null;
    }

    private void readAt(long offset, ByteBuffer buffer) throws IOException {
        while (buffer.hasRemaining()) {
            int count = image.read(buffer, offset);

            if (count <= 0)
                throw new ExtractException("Could not read the disc image (is it complete?).");
            offset += count;
        }
        buffer.flip();
    }

    private static boolean magicAt(ByteBuffer buffer, int offset) {
        for (int index = 0; index < VOLUME_MAGIC.length; index++) {
            if (buffer.get(offset + index) != VOLUME_MAGIC[index])
                return false;
        }
        return true;
    }

    /** the partition's volume descriptor: the root directory's sector and size */
    private long[] findVolume() throws IOException {
        for (long offset : PARTITION_OFFSETS) {
            ByteBuffer descriptor = ByteBuffer.allocate(SECTOR_SIZE).order(ByteOrder.LITTLE_ENDIAN);

            try {
                readAt(offset + VOLUME_DESCRIPTOR_OFFSET, descriptor);
            } catch (IOException e) {
                continue;
            }
            if (!magicAt(descriptor, 0) || !magicAt(descriptor, 0x7EC))
                continue;
            partition = offset;
            return new long[] {
                descriptor.getInt(20) & 0xFFFFFFFFL, descriptor.getInt(24) & 0xFFFFFFFFL,
            };
        }
        throw new ExtractException("This is not an Xbox disc image.");
    }

    /** a directory's table, read whole */
    private ByteBuffer readDirectory(long sector, long size, String what) throws IOException {
        if (size <= 0 || size > MAXIMUM_DIRECTORY_SIZE)
            throw new ExtractException(what);
        ByteBuffer table = ByteBuffer.allocate((int) size).order(ByteOrder.LITTLE_ENDIAN);
        readAt(partition + sector * SECTOR_SIZE, table);
        return table;
    }

    /**
     * collects the entries of the subtree at offset (4-byte units), files or
     * directories as asked
     */
    private static void walk(ByteBuffer table, long offset, int depth, boolean directories, List<Entry> out,
        int[] visited) {
        offset *= 4;
        /* a malformed table must not loop or run off its end */
        if (depth > 64 || ++visited[0] > 4096 || offset + ENTRY_HEADER_SIZE > table.limit())
            return;
        int at = (int) offset;
        int left = table.getShort(at) & 0xFFFF;
        int right = table.getShort(at + 2) & 0xFFFF;
        /* 0xFFFF: padding, an empty directory */
        if (left == 0xFFFF)
            return;
        int nameLength = table.get(at + 13) & 0xFF;
        if (left != 0)
            walk(table, left, depth + 1, directories, out, visited);
        boolean isDirectory = (table.get(at + 12) & ATTRIBUTE_DIRECTORY) != 0;
        if (at + ENTRY_HEADER_SIZE + nameLength <= table.limit() && nameLength > 0 && isDirectory == directories
            && out.size() < MAXIMUM_FILES) {
            byte[] bytes = new byte[nameLength];
            for (int index = 0; index < nameLength; index++)
                bytes[index] = table.get(at + ENTRY_HEADER_SIZE + index);
            String name = new String(bytes, StandardCharsets.ISO_8859_1);
            /* (as extract-xiso refuses them: no name may leave the folder) */
            if (!name.equals(".") && !name.equals("..") && name.indexOf('/') < 0 && name.indexOf('\\') < 0)
                out.add(new Entry(name, table.getInt(at + 4) & 0xFFFFFFFFL, table.getInt(at + 8) & 0xFFFFFFFFL));
        }
        if (right != 0)
            walk(table, right, depth + 1, directories, out, visited);
    }

    private void extract(File destination, Progress progress) throws IOException {
        long[] root = findVolume();

        /* the root's maps folder */
        ByteBuffer table = readDirectory(root[0], root[1], "The disc image's file system is damaged.");
        List<Entry> directories = new ArrayList<>();
        walk(table, 0, 0, true, directories, new int[1]);
        Entry maps = null;
        for (Entry entry : directories) {
            if (entry.name.equalsIgnoreCase("maps"))
                maps = entry;
        }
        if (maps == null)
            throw new ExtractException("The disc image has no maps folder: it is not a Halo disc.");

        /* its files */
        table = readDirectory(maps.sector, maps.size, "The disc image's maps folder is damaged.");
        List<Entry> files = new ArrayList<>();
        walk(table, 0, 0, false, files, new int[1]);
        long total = 0;
        boolean hasUi = false;
        for (Entry file : files) {
            total += file.size;
            hasUi |= file.name.equalsIgnoreCase("ui.map");
        }
        if (!hasUi)
            throw new ExtractException("The disc image's maps folder has no ui.map: it is not a Halo disc.");

        File partial = new File(destination, "maps.partial");
        File finished = new File(destination, "maps");
        if (!partial.isDirectory() && !partial.mkdirs())
            throw new ExtractException("Could not create " + partial + ".");
        ByteBuffer buffer = ByteBuffer.allocateDirect(COPY_BUFFER_SIZE);
        long done = 0;
        for (Entry file : files) {
            File path = new File(partial, file.name);
            long offset = partition + file.sector * SECTOR_SIZE;
            long remaining = file.size;

            try (FileOutputStream out = new FileOutputStream(path)) {
                FileChannel output = out.getChannel();

                while (remaining > 0) {
                    buffer.clear();
                    buffer.limit((int) Math.min(remaining, COPY_BUFFER_SIZE));
                    readAt(offset, buffer);
                    int count = buffer.remaining();
                    while (buffer.hasRemaining())
                        output.write(buffer);
                    offset += count;
                    remaining -= count;
                    done += count;
                    if (!progress.report(file.name, done, total))
                        throw new ExtractException("The copy was stopped.");
                }
            } catch (ExtractException e) {
                throw new ExtractException("Could not read " + file.name + " from the disc image (is it complete?).");
            } catch (IOException e) {
                throw new ExtractException("Could not write " + path + " (is the storage full?).");
            }
        }

        /* (the maps folder may be there, empty: the app makes it for adb) */
        if (!finished.isDirectory() && !finished.mkdirs())
            throw new ExtractException("Could not create " + finished + ".");
        for (Entry file : files) {
            File from = new File(partial, file.name);
            File to = new File(finished, file.name);

            to.delete();
            if (!from.renameTo(to))
                throw new ExtractException("Could not move " + file.name + " into " + finished + ".");
        }
        partial.delete();
    }
}

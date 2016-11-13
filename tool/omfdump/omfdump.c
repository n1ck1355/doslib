
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#ifndef O_BINARY
#define O_BINARY (0)
#endif

const char *omf_rectype_to_str_long(unsigned char rt) {
    switch (rt) {
        case 0x80:  return "Translator Header Record";
        case 0x82:  return "Library Module Header Record";
        case 0x88:  return "Comment Record";
        case 0x8A:  return "Module End Record";
        case 0x8B:  return "Module End Record (32-bit)";
        case 0x8C:  return "External Names Definition Record";
        case 0x90:  return "Public Names Definition Record";
        case 0x91:  return "Public Names Definition Record (32-bit)";
        default:    break;
    }

    return "?";
}

const char *omf_rectype_to_str(unsigned char rt) {
    switch (rt) {
        case 0x80:  return "THEADR";
        case 0x82:  return "LHEADR";
        case 0x88:  return "COMENT";
        case 0x8A:  return "MODEND";
        case 0x8B:  return "MODEND32";
        case 0x8C:  return "EXTDEF";
        case 0x90:  return "PUBDEF";
        case 0x91:  return "PUBDEF32";
        default:    break;
    }

    return "?";
}

static unsigned char        omf_record[16384];
static unsigned char        omf_rectype = 0;
static unsigned long        omf_recoffs = 0;
static unsigned int         omf_reclen = 0;

static char*                in_file = NULL;   

static void help(void) {
    fprintf(stderr,"omfdump [options]\n");
    fprintf(stderr,"  -i <file>    OMF file to dump\n");
}

static int read_omf_record(int fd) {
    unsigned char sum = 0;
    unsigned int i;

    omf_recoffs = lseek(fd,0,SEEK_CUR);
    if (read(fd,omf_record,3) != 3)
        return 0;

    omf_rectype = omf_record[0];
    omf_reclen = *((uint16_t*)(omf_record+1));
    if (omf_reclen > sizeof(omf_record))
        return 0;

    if (omf_reclen > 0) {
        for (i=0;i < 3;i++)
            sum += omf_record[i];

        if ((unsigned int)read(fd,omf_record,omf_reclen) != omf_reclen)
            return 0;

        if (omf_record[omf_reclen-1]/* checksum */ != 0) {
            for (i=0;i < omf_reclen;i++)
                sum += omf_record[i];

            if (sum != 0) {
                fprintf(stderr,"* checksum failed sum=0x%02x\n",sum);
                return 0;
            }
        }
    }

    return 1;
}

int main(int argc,char **argv) {
    int i,fd;
    char *a;

    for (i=1;i < argc;) {
        a = argv[i++];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"i")) {
                in_file = argv[i++];
                if (in_file == NULL) return 1;
            }
            else {
                help();
                return 1;
            }
        }
        else {
            fprintf(stderr,"Unexpected arg %s\n",a);
            return 1;
        }
    }

    if (in_file == NULL) {
        help();
        return 1;
    }

    fd = open(in_file,O_RDONLY|O_BINARY);
    if (fd < 0) {
        fprintf(stderr,"Failed to open input file %s\n",strerror(errno));
        return 1;
    }

    while (read_omf_record(fd)) {
        printf("OMF record type=0x%02x (%s: %s) length=%u offset=%lu\n",
            omf_rectype,
            omf_rectype_to_str(omf_rectype),
            omf_rectype_to_str_long(omf_rectype),
            omf_reclen,
            omf_recoffs);
    }

    close(fd);
    return 0;
}


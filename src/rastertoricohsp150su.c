// Ricoh SP 150su raster driver for cups-filters.

/*
Инфо
1 пункт равен 1/72 дюйма (1pt = 1/72 inc)
1 дюйм равен 25,4 мм

Raster format:
PageSize (W,H) представляет размер страницы в пунктах
cupsWidth cupsHeight - представляет размер той же страницы но в пикселях
при отсутвии размера PageSize его можно восстановить из ширины и высоты
PageSize W = cupsWidth * 72 / HWResolution
*/

/* TODO
WARN    Размер "16K" должен быть в формате Adobe standard name "184.86x260mm".
WARN    Размер "A5LEF" должен быть в формате Adobe standard name "A5Rotated".
WARN    Размер "B6LEF" должен быть в формате Adobe standard name "B6Rotated".
*/

#include <config.h>
#include <cupsfilters/driver.h>
#include <ppd/ppd.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <bits/local_lim.h>

#include <jbig.h>

union uLPHL
{ // for language printer
    unsigned char bHeader[64];
    unsigned short sHeader[32];
    unsigned int Header[16];
};

//
// Globals...
//

int Canceled;                     // Is the job canceled?
unsigned int FWPaperWidthInPixel; // Firmware width size page in pixel -> lhplHeader.Header[2]
int pagemodel;                    // Model page from pageSize
int duplexMode;                   // duplex-tumble mode
int isOddPage;                    // Odd check for duplex
const int IsSWIFT = 1;            // not change
unsigned int OutputCount;         // Counter of bytes written to stdout
int page;                         // Page count printed
unsigned char *lineBuff;          // Line buffer
unsigned char *bitBuffK;          // K buffer  (CMYK)
unsigned int lenBuffK;
unsigned char *JbigCompressDataK;
unsigned int JbigCompressDataLenK;
char tmpFileBuff[10][0x400];
int PaperWidthMarginInPixel[12] = {
    0x1326, // Letter
    0x1298, // A4
    0x0ce6, // A5
    0x08e5,
    0x1006,
    0x0b0b,
    0x1038,
    0x1048,
    0x1298,
    0x1006,
    0x1326,
    0x0000 // Custom size
};
int PaperHeightMarginInPixel[12] = {
    0x19c8,
    0x1b68,
    0x135e,
    0x0dac,
    0x17bb,
    0x10cc,
    0x189c,
    0x17fd,
    0x0dac,
    0x0bd1,
    0x20d0,
    0x0000 // Custom size
};
union uLPHL gdiStartPage, gdiEndPage;

static unsigned char dither_noise[16][16] = // Blue-noise dither array
    {
        {111, 49, 142, 162, 113, 195, 71, 177, 201, 50, 151, 94, 66, 37, 85, 252},
        {25, 99, 239, 222, 32, 250, 148, 19, 38, 106, 220, 170, 194, 138, 13, 167},
        {125, 178, 79, 15, 65, 173, 123, 87, 213, 131, 247, 23, 116, 54, 229, 212},
        {41, 202, 152, 132, 189, 104, 53, 236, 161, 62, 1, 181, 77, 241, 147, 68},
        {2, 244, 56, 91, 230, 5, 204, 28, 187, 101, 144, 206, 33, 92, 190, 107},
        {223, 164, 114, 36, 214, 156, 139, 70, 245, 84, 226, 48, 126, 158, 17, 135},
        {83, 196, 21, 254, 76, 45, 179, 115, 12, 40, 169, 105, 253, 176, 211, 59},
        {100, 180, 145, 122, 172, 97, 235, 129, 215, 149, 199, 8, 72, 26, 238, 44},
        {232, 31, 69, 11, 205, 58, 18, 193, 88, 60, 112, 221, 140, 86, 120, 153},
        {208, 130, 243, 160, 224, 110, 34, 248, 165, 24, 234, 184, 52, 198, 171, 6},
        {108, 188, 51, 89, 137, 186, 154, 78, 47, 134, 98, 157, 35, 249, 95, 63},
        {16, 75, 219, 39, 0, 67, 228, 121, 197, 240, 3, 74, 127, 20, 227, 143},
        {246, 175, 119, 200, 251, 103, 146, 14, 209, 174, 109, 218, 192, 82, 203, 163},
        {29, 93, 150, 22, 166, 182, 55, 30, 90, 64, 42, 141, 168, 57, 117, 46},
        {216, 233, 61, 128, 81, 237, 217, 118, 159, 255, 185, 27, 242, 102, 4, 133},
        {73, 191, 9, 210, 43, 96, 7, 136, 231, 80, 10, 124, 225, 207, 155, 183}};

// // Set dither arrays...
// for (i = 0; i < 16; i ++)
// {
//     // Apply gamma correction to dither array...
//     for (j = 0; j < 16; j ++)
//     dither[i][j] = 255 - (int)(255.0 * pow(1.0 - dither_noise[i][j] / 255.0, 0.4545));
// }
// /mnt/disk/code/ricoh/pappl/pappl-1.4.x/pappl/job-filter.c

//  **************************************** //
// '*checkedmalloc()' - Helper for malloc .. //
//  **************************************** //

static void *checkedmalloc(size_t n)
{
    void *p;

    if ((p = malloc(n)) == NULL)
    {
        fprintf(stderr, "Sorry, not enough memory available!\n");
        exit(1);
    }

    return p;
}

//  *********************************************** //
// 'WriteToDevice()' - Function helper for writen.. //
//  *********************************************** //

int WriteToDevice(const char *buf, FILE *desc)
{
    int len = strlen(buf);
    fwrite(buf, len, 1, desc);
    return len;
}

//  **************************************** //
// 'CancelJob()' - Cancel the current job... //
//  **************************************** //

void CancelJob(int sig) // I - Signal
{
    (void)sig;

    Canceled = 1;
}

static void pushJBIGData(unsigned char *start, size_t len, void *dummy)
{
    memcpy(JbigCompressDataK + JbigCompressDataLenK, start, len);
    JbigCompressDataLenK += (unsigned int)len;

    return;
}

//  ******************************************** //
// 'JBIGCompress()' - jbig compress data page... //
//  ******************************************** //

int JBIGCompress(int width, int height, unsigned char *buff, int planes)
{
    unsigned char **image;
    long uVar1;
    struct jbg_enc_state state;

    if (planes != 0)
        return 0;

    image = (unsigned char **)checkedmalloc(sizeof(unsigned char *));
    image[0] = buff;

    jbg_enc_init(&state, width, height, 1, image, pushJBIGData, NULL);
    jbg_enc_layers(&state, 0);
    jbg_enc_options(&state, 0, 0x340, 0x80, 0, 0);
    jbg_enc_out(&state);
    jbg_enc_free(&state);
    free(image);

    return 1;
}

//  **************************************** //
// 'Setup()' - Initialization printer job... //
//  **************************************** //

void Setup(cups_page_header2_t *header, unsigned char b, char *doc, char *user)
{
    if (!header)
        return;
    fprintf(stderr, "DEBUG: Document \"%s\" from user %s.\n", doc, user);

    const char *com_UEL = "\033%-12345X";
    const char *pjlJobName = "@PJL JOB NAME=PRINTER\r\n";
    const char *pjlJobAttr = "@PJL SET JOBATTR=%s:%s\r\n";
    const char *pjlDuplex = "@PJL SET DUPLEX=%s\r\n";
    const char *pjlSet = "@PJL SET %s=%d\r\n";
    const char *pjLanguage = "@PJL ENTER LANGUAGE=LHPL\r\n";
    char writeStr[254];
    char *tmpStr;
    char tmpStr16[16];
    int tmpInt;
    union uLPHL startJob;

    FILE *fd;
    fd = stdout;

    OutputCount += WriteToDevice(com_UEL, fd);

    OutputCount += WriteToDevice(pjlJobName, fd);

    char hostname[HOST_NAME_MAX + 1];
    gethostname(hostname, HOST_NAME_MAX + 1);

    sprintf(writeStr, pjlJobAttr, "HST", hostname);
    OutputCount += WriteToDevice(writeStr, fd);

    if (strlen(user) > 225)
        user[225] = '\0';
    sprintf(writeStr, pjlJobAttr, "USR", user);
    OutputCount += WriteToDevice(writeStr, fd);
    if (strlen(doc) > 225)
        doc[225] = '\0';
    sprintf(writeStr, pjlJobAttr, "DOC", doc);
    OutputCount += WriteToDevice(writeStr, fd);

    time_t mt = time(NULL);
    struct tm *now = localtime(&mt);

    sprintf(tmpStr16, "%02d/%02d/%04d", now->tm_mday, now->tm_mon + 1, now->tm_year + 1900);
    sprintf(writeStr, pjlJobAttr, "DATE", tmpStr16);
    OutputCount += WriteToDevice(writeStr, fd);

    sprintf(tmpStr16, "%02d:%02d:%02d", now->tm_hour, now->tm_min, now->tm_sec);
    sprintf(writeStr, pjlJobAttr, "TIME", tmpStr16);
    OutputCount += WriteToDevice(writeStr, fd);

    if (duplexMode == 0)
    {
        sprintf(writeStr, pjlDuplex, "OFF");
        OutputCount += WriteToDevice(writeStr, fd);
    }
    else
    {
        sprintf(writeStr, pjlDuplex, "ON");
        OutputCount += WriteToDevice(writeStr, fd);

        if (header->Collate == CUPS_TRUE)
        {
            tmpInt = (page * header->NumCopies >> 1);
        }
        else
        {
            tmpInt = (page >> 1);
        }
        sprintf(writeStr, pjlSet, "MDPXS", tmpInt + 1);
        OutputCount += WriteToDevice(writeStr, fd);

        if (isOddPage)
        { // добавлена пустая страница
            sprintf(tmpStr16, "%d", page - 1);
        }
        else
        {
            sprintf(tmpStr16, "%d", page);
        }
        sprintf(writeStr, pjlJobAttr, "PCNT", tmpStr16);
        OutputCount += WriteToDevice(writeStr, fd);
    }

    sprintf(writeStr, "@PJL SET MEDIASOURCE=%d\r\n", 0);
    OutputCount += WriteToDevice(writeStr, fd);

    if (header->cupsBitsPerPixel == 8)
    {
        tmpStr = "GRAYSCALE";
    }
    else
    {
        tmpStr = "COLOR";
    }
    sprintf(writeStr, "@PJL SET RENDERMODE=%s\r\n", tmpStr);
    OutputCount += WriteToDevice(writeStr, fd);

    sprintf(writeStr, pjlSet, "RESOLUTION", header->HWResolution[0]);
    OutputCount += WriteToDevice(writeStr, fd);

    if (IsSWIFT == 0)
    {
        tmpInt = 2;
    }
    else
    {
        tmpInt = 1;
    }
    sprintf(writeStr, pjlSet, "BITSPERPIXEL", tmpInt);
    OutputCount += WriteToDevice(writeStr, fd);

    if (header->Collate == CUPS_TRUE) // TODO: Пока нет обработки детсвий в драйвере
    {
        tmpStr = "QTY";
        tmpInt = 1;
    }
    else
    {
        tmpInt = header->NumCopies;
        tmpStr = "COPIES";
    }
    sprintf(writeStr, pjlSet, tmpStr, tmpInt);
    OutputCount += WriteToDevice(writeStr, fd);

    OutputCount += WriteToDevice(pjLanguage, fd);

    memset(&startJob, 0, 0x40);
    startJob.Header[0] = 0x40484c1b;
    startJob.sHeader[2] = 0x6a73;        // sj -start job
    startJob.bHeader[6] = b;             // always equal to 1
    startJob.sHeader[4] = (short)tmpInt; // Contains the number of copies
    for (int i = 0; i < 63; i++)
        startJob.bHeader[63] ^= startJob.bHeader[i];

    fwrite(&startJob, 0x40, 1, fd);
}

//  ************************************ //
// 'End()' - Finalization printer job... //
//  ************************************ //

void End(int status)
{
    const char *com_UEL = "\033%-12345X";
    const char *pjLEnd = "@PJL EOJ\r\n";
    FILE *fd;

    fd = stdout;
    OutputCount += WriteToDevice(com_UEL, fd);
    OutputCount += WriteToDevice(pjLEnd, fd);

    fputs("DEBUG: Finalization printing!\n", stderr);
}

//  ******************************************************* //
// 'ProcessLine()' - Preparete line pixels and dithering... //
//  ******************************************************* //
/*Read, prepare (dithering) and write a line to bit buffer*/
void ProcessLine(cups_raster_t *ras, cups_page_header2_t *header, int nLine)
{
    unsigned int ret =
        cupsRasterReadPixels(ras, lineBuff, header->cupsBytesPerLine);

    if (ret == 0)
    {
        fputs("ERROR: Unable read raster data!\n", stderr);
    }

    // Кол-во полных байт занимаемых битами данных
    unsigned int nLineInByte = (FWPaperWidthInPixel + 7) >> 3;
    // Определяем указатель на выходные данные исходя из номера строки и кол-ва байт данных выходной строки
    unsigned char *bitLine = bitBuffK + nLineInByte * nLine;
    // Обнуляем значения выходной строки
    memset(bitLine, 0, nLineInByte);
    unsigned char *dither = dither_noise[nLine & 15];
    if (header->cupsBitsPerPixel == 8)
    {
        unsigned char bit = 128, byte = 0;
        if (header->cupsColorSpace == CUPS_CSPACE_K)
        {
            // 8 bit black
            for (int x = 0; x < header->cupsBytesPerLine; x++)
            {
                if (lineBuff[x] >= dither[x & 15])
                    byte |= bit;

                if (bit == 1)
                {
                    *bitLine++ = byte;
                    byte = 0;
                    bit = 128;
                }
                else
                    bit /= 2;
            }

            if (bit < 128)
                *bitLine = byte;
        }
        else
        {
            // 8 bit gray
            for (int x = 0; x < header->cupsBytesPerLine; x++)
            {
                if (lineBuff[x] < dither[x & 15])
                    byte |= bit;

                if (bit == 1)
                {
                    *bitLine++ = byte;
                    byte = 0;
                    bit = 128;
                }
                else
                    bit /= 2;
            }

            if (bit < 128)
                *bitLine = byte;
        }
    }
    else if (header->cupsBitsPerPixel == 1)
    {
        // 1-bit B&W
        memcpy(bitLine, lineBuff, (header->cupsBytesPerLine + 7) >> 3);
    }
}

//  ***************************************************** //
// 'PrintPageFile()' - write printing page to TMP file... //
//  ***************************************************** //

void PrintPageFile(cups_page_header2_t *header)
{

    size_t maxSiziJbig = (FWPaperWidthInPixel >> 3) * header->cupsHeight;

    unsigned char paperModel[12] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 9, 0};

    JbigCompressDataLenK = 0;
    JbigCompressDataK = (unsigned char *)checkedmalloc(maxSiziJbig);
    // Упаковать данные в JBIG
    JBIGCompress(FWPaperWidthInPixel, header->cupsHeight, bitBuffK, 0);
    // prepare gdiStartPage and gdiEndPage
    // TODO

    gdiStartPage.Header[0] = 0x40484c1b; // Magic word size 4 byte
    gdiStartPage.sHeader[2] = 0x7073;    // sp -start page
    gdiStartPage.bHeader[6] = paperModel[pagemodel];
    gdiStartPage.bHeader[7] = header->cupsMediaType <= 5 ? header->cupsMediaType + 1 : 1; // MediaType
    gdiStartPage.Header[5] = JbigCompressDataLenK;
    gdiStartPage.Header[6] = JbigCompressDataLenK;
    gdiStartPage.Header[2] = FWPaperWidthInPixel;
    gdiStartPage.Header[3] = header->cupsHeight;
    gdiStartPage.Header[4] = maxSiziJbig;
    gdiStartPage.sHeader[21] = (unsigned short)header->HWResolution[0];
    gdiStartPage.Header[22] = (header->PageSize[0] * 0xfe) / 0x48;
    gdiStartPage.Header[23] = (header->PageSize[1] * 0xfe) / 0x48;

    for (int i = 0; i < 63; i++)
        gdiStartPage.bHeader[63] ^= gdiStartPage.bHeader[i];

    gdiEndPage.Header[0] = 0x40484c1b;
    gdiEndPage.sHeader[2] = 0x7065; // ep -end page

    for (int i = 0; i < 63; i++)
        gdiEndPage.bHeader[63] ^= gdiEndPage.bHeader[i];

    int tempFd = cupsTempFd(tmpFileBuff[page], lenBuffK);
    if (tempFd == -1)
    {
        exit(1);
    }
    fprintf(stderr, "DEBUG:Create cups temp file = \"%s\"\n", tmpFileBuff[page]);

    // write gdiStartPage
    write(tempFd, &gdiStartPage, 0x40);
    // write page data JBIG     NOT bitBuffK lenBuffK
    write(tempFd, JbigCompressDataK, JbigCompressDataLenK);
    // write gdiEndpage
    write(tempFd, &gdiEndPage, 0x40);
    close(tempFd);
    free(JbigCompressDataK);
    fprintf(stderr, "DEBUG: Compress JBIG data from %d bytes to %d bytes.\n", lenBuffK, JbigCompressDataLenK);
}

//  ********************************************** //
// 'main()' - Main entry and processing of driver. //
//  ********************************************** //

int                // O - Exit status
main(int argc,     // I - Number of command-line arguments
     char *argv[]) // I - Command-line arguments
{
    int fd;                     // File descriptor
    int empty = 1;              // Empty flag
    cups_raster_t *ras;         // Raster stream for printing
    cups_page_header2_t header, // Page header from file
        lastHeader;
    int y;                  // Current line
    ppd_file_t *ppd;        // PPD file
    int num_options;        // Number of options
    cups_option_t *options; // Options
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
    struct sigaction action; // Actions for POSIX signals
#endif                       // HAVE_SIGACTION && !HAVE_SIGSET

    int dpi;                 // DPI
    unsigned int tonermode;  // Toner mode
    char *valueName;         // search value name
    ppd_choice_t *ppdChoice; // Find choice

    //
    // Make sure status messages are not buffered...
    //
    setbuf(stderr, NULL);

    //
    // Check command-line...
    //
    if (argc < 6 || argc > 7)
    {
        fprintf(stderr, "Usage: %s job-id user title copies options [file]\n",
                argv[0]);
        return (1);
    }

    num_options = cupsParseOptions(argv[5], 0, &options);

    //
    // Open the PPD file...
    //
    ppd = ppdOpenFile(getenv("PPD"));

    if (!ppd)
    {
        ppd_status_t status; // PPD error
        int linenum;         // Line number

        fputs("ERROR: The PPD file could not be opened.\n", stderr);

        status = ppdLastError(&linenum);

        fprintf(stderr, "DEBUG: %s on line %d.\n", ppdErrorString(status), linenum);

        return (1);
    }

    ppdMarkDefaults(ppd);
    ppdMarkOptions(ppd, num_options, options);

    ppdChoice = ppdFindMarkedChoice(ppd, "PageSize");
    if (ppdChoice)
    {
        fprintf(stderr, "DEBUG: Find PageSize in ppdFindMarkedChoice(): %s.\n", ppdChoice->choice);
        valueName = ppdChoice->choice;
    }
    else
    {
        fprintf(stderr, "DEBUG: ppdFindMarkedChoice: not finder.\n");
        valueName = (char *)cupsGetOption("PageSize", num_options, options);
        fprintf(stderr, "DEBUG: cupsGetOption: %s.\n", valueName);
    }

    if (valueName)
    {
        if (strcmp(valueName, "Letter") == 0) // 8,5x11inc - 216x279mm
            pagemodel = 0;
        else if (strcmp(valueName, "A4") == 0) // 210x297mm
            pagemodel = 1;
        else if (strcmp(valueName, "A5") == 0) // 148x210mm
            pagemodel = 2;
        else if (strcmp(valueName, "A6") == 0) // 105x148mm
            pagemodel = 3;
        else if (strcmp(valueName, "B5") == 0) //
            pagemodel = 4;
        else if (strcmp(valueName, "B6") == 0) //
            pagemodel = 5;
        else if (strcmp(valueName, "Executive") == 0) //
            pagemodel = 6;
        else if (strcmp(valueName, "16K") == 0) //
            pagemodel = 7;
        else if (strcmp(valueName, "A5LEF") == 0) // Поперечный т.е. подача листа боком
            pagemodel = 8;
        else if (strcmp(valueName, "B6LEF") == 0)
            pagemodel = 9;
        else if (strcmp(valueName, "Legal") == 0)
            pagemodel = 10;
        else if (strcmp(valueName, "Custom") == 0)
            pagemodel = 11;
    }
    else
    {
        pagemodel = 11;
    }
    fprintf(stderr, "DEBUG: Select pagemodel: %i.\n", pagemodel);

    ppdChoice = ppdFindMarkedChoice(ppd, "DrvResolution");
    if (ppdChoice)
    {
        if (strcmp(ppdChoice->choice, "600dpi") == 0)
            dpi = 600;
        else
            dpi = 1200;
    }
    else
    {
        dpi = 600;
    }
    fprintf(stderr, "DEBUG: DrvResolution: %idpi.\n", dpi);

    ppdChoice = ppdFindMarkedChoice(ppd, "TonerMode");
    if (ppdChoice)
    {
        if (strcmp(ppdChoice->choice, "3") == 0)
        {
            tonermode = 3;
        }
        else if (strcmp(ppdChoice->choice, "2") == 0)
        {
            tonermode = 2;
        }
        else
        {
            tonermode = (unsigned int)(strcmp(ppdChoice->choice, "1") == 0);
        }
    }
    fprintf(stderr, "DEBUG: TonerMode: %i.\n", tonermode);

    //
    // Open the page stream...
    //
    if (argc == 7)
    {
        if ((fd = open(argv[6], O_RDONLY)) == -1)
        {
            perror("ERROR: Unable to open raster file");
            return (1);
        }
    }
    else
        fd = 0;

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);

    //
    // Register a signal handler to eject the current page if the
    // job is cancelled.
    //
    Canceled = 0;

#ifdef HAVE_SIGSET // Use System V signals over POSIX to avoid bugs
    sigset(SIGTERM, CancelJob);
#elif defined(HAVE_SIGACTION)
    memset(&action, 0, sizeof(action));

    sigemptyset(&action.sa_mask);
    action.sa_handler = CancelJob;
    sigaction(SIGTERM, &action, NULL);
#else
    signal(SIGTERM, CancelJob);
#endif // HAVE_SIGSET

    //
    // Process pages as needed...
    //
    page = 0;

    // Test temp file create
    if (cupsTempFd(tmpFileBuff[page], 0) < 0)
    {
        // TODO: check err == -1 and exit
        return -1;
    }

    while (cupsRasterReadHeader2(ras, &header))
    {
        memcpy(&lastHeader, &header, sizeof(cups_page_header2_t)); // copy last header for use outside the loop in Setup()
        if (empty)                                                 // It`s first loop, header GOOD
        {
            empty = 0;
        }

        // Reset Start and End data
        memset(&gdiStartPage, 0, 0x40);
        memset(&gdiEndPage, 0, 0x40);

        fprintf(stderr, "DEBUG: Duplex = %d\n", header.Duplex);
        fprintf(stderr, "DEBUG: Tumble = %d\n", header.Tumble);
        // duplexMode = 0(NoDuplexNoTumble), 1(DuplexNoTumble), 2(DuplexTumble)
        duplexMode = header.Duplex ? (1 + (header.Tumble) ? 1 : 0) : 0;
        fprintf(stderr, "DEBUG: duplexMode = %d\n", duplexMode);

        // PREPARING THE PAGE TITLE
        if (duplexMode == 0)
        {
            if (header.PageSize[1] < header.PageSize[0])
            {
                if ((pagemodel == 3) || (0x264 < header.PageSize[0] && pagemodel != 0xb))
                {
                    unsigned int tmp = header.PageSize[1];
                    header.PageSize[1] = header.PageSize[0];
                    header.PageSize[0] = tmp;
                }
            }

            int pageWidthMarginInPixel = (header.PageSize[0] * 600) / 72 - 198;
            int pageHeightMarginInPixel = (header.PageSize[1] * 600) / 72;
            if ((pageWidthMarginInPixel != PaperWidthMarginInPixel[pagemodel]) &&
                (pageHeightMarginInPixel != PaperHeightMarginInPixel[pagemodel]))
                pagemodel = 11;

            switch (pagemodel)
            {
            case 0:
                FWPaperWidthInPixel = (header.HWResolution[0] * 0x1400) / 600;
                gdiStartPage.bHeader[6] = 1;
                break;
            case 1:
                FWPaperWidthInPixel = (header.HWResolution[0] * 0x1400) / 600;
                gdiStartPage.bHeader[6] = 2;
                break;
            case 2:
                if (header.PageSize[1] < header.PageSize[0])
                {
                    pagemodel = 8;
                    FWPaperWidthInPixel = (header.HWResolution[0] * 0x1400) / 600;
                    gdiStartPage.bHeader[6] = 10;
                }
                else
                {
                    FWPaperWidthInPixel = (header.HWResolution[0] * 0xe00) / 600;
                    gdiStartPage.bHeader[6] = 3;
                }
                break;
            case 3:
                FWPaperWidthInPixel = (header.HWResolution[0] * 0xa00) / 600;
                gdiStartPage.bHeader[6] = 4;
                break;
            case 4:
                FWPaperWidthInPixel = (header.HWResolution[0] * 0x1200) / 600;
                gdiStartPage.bHeader[6] = 5;
                break;
            case 5:
                if (header.PageSize[1] < header.PageSize[0])
                {
                    pagemodel = 9;
                    FWPaperWidthInPixel = (header.HWResolution[0] * 0x1200) / 600;
                    gdiStartPage.bHeader[6] = 11;
                }
                else
                {
                    FWPaperWidthInPixel = (header.HWResolution[0] * 0xc00) / 600;
                    gdiStartPage.bHeader[6] = 6;
                }
                break;
            case 6:
                FWPaperWidthInPixel = (header.HWResolution[0] * 0x1200) / 600;
                gdiStartPage.bHeader[6] = 7;
                break;
            case 7:
                FWPaperWidthInPixel = (header.HWResolution[0] * 0x1200) / 600;
                gdiStartPage.bHeader[6] = 8;
                break;
            case 8:
                if (header.PageSize[0] < header.PageSize[1])
                {
                    pagemodel = 2;
                    FWPaperWidthInPixel = (header.HWResolution[0] * 0xe00) / 600;
                    gdiStartPage.bHeader[6] = 3;
                }
                else
                {
                    FWPaperWidthInPixel = (header.HWResolution[0] * 0x1400) / 600;
                    gdiStartPage.bHeader[6] = 10;
                }
                break;
            case 9:
                if (header.PageSize[0] < header.PageSize[1])
                {
                    pagemodel = 5;
                    FWPaperWidthInPixel = (header.HWResolution[0] * 0xc00) / 600;
                    gdiStartPage.bHeader[6] = 6;
                }
                else
                {
                    FWPaperWidthInPixel = (header.HWResolution[0] * 0x1200) / 600;
                    gdiStartPage.bHeader[6] = 11;
                }
                break;
            case 10:
                FWPaperWidthInPixel = (header.HWResolution[0] * 0x1400) / 600;
                gdiStartPage.bHeader[6] = 9;
                break;
            case 11:
            default:
                PaperWidthMarginInPixel[11] = pageWidthMarginInPixel;
                unsigned int uVar36 = (header.PageSize[0] / 72.0) * header.HWResolution[0];
                // if ((FWPaperWidthInPixel & 0x1ff)){
                //     if (-1 < (int)(FWPaperWidthInPixel + 0x1ff)) {
                //         uVar10 = uVar36;
                //     }
                // }
                if ((uVar36 & 0x1ff) == 0)
                { // should be a multiple of 0x200
                    FWPaperWidthInPixel = (header.PageSize[0] / 72.0) * header.HWResolution[0];
                }
                else
                {
                    unsigned int uVar10 = uVar36 + 0x1ff;
                    if (-1 < (int)uVar36)
                    {
                        uVar10 = uVar36;
                    }
                    FWPaperWidthInPixel = (uVar10 & 0xfffffe00) + 0x200;
                }
                gdiStartPage.bHeader[6] = 0;
            }
            fprintf(stderr, "DEBUG: Firmware width: %i.\n", FWPaperWidthInPixel);
            fprintf(stderr, "DEBUG: Header sizePage byte: %i.\n", gdiStartPage.bHeader[6]);

            gdiStartPage.Header[2] = FWPaperWidthInPixel & 0x0000ffff;
            gdiStartPage.Header[11] = (((unsigned int)((unsigned long)(header.PageSize[1] * 0xfe) / 0x48)) << 16) |
                                      (short)((unsigned long)(header.PageSize[0] * 0xfe) / 0x48);

            fprintf(stderr, "DEBUG: TEST width in mm: %i.\n", (short)((unsigned long)(header.PageSize[0] * 0xfe) / 0x48));
            fprintf(stderr, "DEBUG: TEST height in mm: %i.\n", (short)((unsigned long)(header.PageSize[1] * 0xfe) / 0x48));
            fprintf(stderr, "DEBUG: TEST  lhplHeader.Header[2]: 0x%X.\n", gdiStartPage.Header[2]);
            fprintf(stderr, "DEBUG: TEST  lhplHeader.Header[11]: 0x%X.\n", gdiStartPage.Header[11]); // 6F08 EA0A - big for "Leter"

            // Allocate memory for buffers Line
            if (header.cupsBitsPerPixel == 8)
            {
                lineBuff = (unsigned char *)checkedmalloc((size_t)FWPaperWidthInPixel);
                lenBuffK = ((FWPaperWidthInPixel + 7) >> 3) * header.cupsHeight;
                bitBuffK = calloc(lenBuffK, sizeof(unsigned char));
            }
            else
                exit(1);
        }

        //
        // Write a status message with the page number and number of copies.
        //

        if (Canceled)
            break;

        page++;

        fprintf(stderr, "PAGE: %d 1\n", page);
        fprintf(stderr, "INFO: Starting page %d.\n", page);

        for (y = 0; y < header.cupsHeight; y++)
        {

            if (Canceled)
                break;

            // Print message for progress printing
            if ((y & 127) == 0)
            {
                fprintf(stderr, "INFO: Printing page %d, %d%% complete.\n",
                        page, 100 * y / header.cupsHeight);
                fprintf(stderr, "ATTR: job-media-progress=%d\n",
                        100 * y / header.cupsHeight);
            }

            /*Read, prepare (dithering) and write a line to bitBuffK*/
            ProcessLine(ras, &header, y);
        }

        //
        // Eject the page...
        // Pack and write page to tmp file
        //
        PrintPageFile(&header);

        free(lineBuff);

        fprintf(stderr, "INFO: Finished page %d.\n", page);

        if (Canceled)
            break;
    }

    if (!empty & !Canceled) // THE DATA REDY TO PRINT TO DEVICE
    {
        //
        // Initialize the print device... and job
        // Формирование документа и печать из файла на устройство

        Setup(&lastHeader, 1, argv[3], argv[2]);

        for (int i = 1; i <= page; i++)
        {
            int fout = open(tmpFileBuff[i], O_RDONLY);
            int sSize = 0;
            size_t sWrite = 0;
            unsigned char tmpBuff[0x8000];
            while (sSize = read(fout, tmpBuff, 0x8000), sSize > 0)
            {
                do
                {
                    sWrite = fwrite(&tmpBuff, sSize, 1, stdout);
                } while (sWrite == 0);
            }
            close(fout);
        }

        End(0);

        free(bitBuffK);
    }

    // if (!empty)
    //     Shutdown(ppd);

    cupsFreeOptions(num_options, options);

    cupsRasterClose(ras);

    if (fd != 0)
        close(fd);

    for (int i = page; i >= 0; i--)
    {
        remove(tmpFileBuff[i]);
        fprintf(stderr, "DEBUG: Remove temp file: %s\n", tmpFileBuff[i]);
    }

    // if (DotBuffers[0] != NULL)
    //     free(DotBuffers[0]);

    if (empty)
    {
        fprintf(stderr, "DEBUG: Input is empty, outputting empty file.\n");
        return 0;
    }
    return (page == 0);
}

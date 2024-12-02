#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>
#include <LittleFS.h>
#include <StreamUtils.h>

// #define DEBUG

#define HALT()                                                                 \
  {                                                                            \
    digitalWrite(LED, HIGH);                                           \
    while (true) {                                                             \
      yield();                                                                 \
    }                                                                          \
  }

const uint8_t LED = 48;

TFT_eSPI tft = TFT_eSPI();

using namespace fs;

AnimatedGIF gif;
File file;
uint16_t* framebuf = nullptr;

void* GIFOpenFile(const char* fname, int32_t* pSize) {
  file = LittleFS.open(fname);
  if (file) {
    *pSize = file.size();
    return (void*)&file;
  }
  return nullptr;
}

void GIFCloseFile(void* pHandle) {
  File* f = static_cast<File*>(pHandle);
  if (f != nullptr) {
    f->close();
    delete f;
  }
}

int32_t GIFReadFile(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  int32_t iBytesRead;
  iBytesRead = iLen;
  File* f = static_cast<File*>(pFile->fHandle);
  // Note: If you read a file all the way to the last byte, seek() stops working
  if ((pFile->iSize - pFile->iPos) < iLen) {
    iBytesRead = pFile->iSize - pFile->iPos - 1; // <-- ugly work-around
  }
  if (iBytesRead <= 0) {
    return 0;
  }
  iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
  pFile->iPos = f->position();
  return iBytesRead;
}

int32_t GIFSeekFile(GIFFILE* pFile, int32_t iPosition) {
  File* f = static_cast<File*>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// https://github.com/bitbank2/AnimatedGIF/blob/master/examples/TFT_eSPI_memory/GIFDraw.ino
#define BUFFER_SIZE 256            // Optimum is >= GIF width or integral division of width

#ifdef USE_DMA
uint16_t usTemp[2][BUFFER_SIZE]; // Global to support DMA use
#else
uint16_t usTemp[1][BUFFER_SIZE];    // Global to support DMA use
#endif
bool     dmaBuf = 0;

void GIFDraw(GIFDRAW* pDraw) {
  uint8_t* s;
  uint16_t *d, *usPalette;
  int x, iWidth;
  
  iWidth = pDraw->iWidth;
  if (iWidth + pDraw->iX > tft.width())
    iWidth = tft.width() - pDraw->iX;
  usPalette = pDraw->pPalette;
  if (pDraw->iY + pDraw->y >= tft.height() || pDraw->iX >= tft.width() ||
      iWidth < 1)
    return;
  if (pDraw->y == 0) { // start of frame, set address window on LCD
    tft.dmaWait();     // wait for previous writes to complete before trying to
                       // access the LCD
    tft.setAddrWindow(pDraw->iX, pDraw->iY, pDraw->iWidth, pDraw->iHeight);
    // By setting the address window to the size of the current GIF frame, we
    // can just write continuously over the whole frame without having to set
    // the address window again
  }
  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) // restore to background color
  {
    for (x = 0; x < iWidth; x++) {
      if (s[x] == pDraw->ucTransparent)
        s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }

  // Apply the new pixels to the main image
  d = &framebuf[pDraw->iWidth * pDraw->y];
  if (pDraw->ucHasTransparency) // if transparency used
  {
    uint8_t c, ucTransparent = pDraw->ucTransparent;
    int x;
    for (x = 0; x < iWidth; x++) {
      c = *s++;
      if (c != ucTransparent)
        d[x] = usPalette[c];
    }
  } else {
    // Translate the 8-bit pixels through the RGB565 palette (already byte
    // reversed)
    for (x = 0; x < iWidth; x++) {
      d[x] = usPalette[s[x]];
    }
  }
  tft.dmaWait(); // wait for last write to complete (the last scan line)
  // We write with block set to FALSE (3rd param) so that we can be decoding the
  // next line while the DMA hardware continues to write data to the LCD
  // controller
  tft.pushPixels(d, iWidth);
}

[[noreturn]] void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);
  
  tft.init();
  tft.setSwapBytes(true);
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, 0);
  tft.setTextSize(1);
  tft.setTextWrap(true);
  WriteLoggingStream logger(Serial, tft);
  
  if (!LittleFS.begin()) {
    logger.println("Failed to mount file system");
    HALT();
  }

  const uint16_t framebufSize = tft.width() * tft.height() * sizeof(uint16_t);
  framebuf = (uint16_t*)malloc(framebufSize);
  if (framebuf == nullptr) {
    logger.printf("Failed to allocate framebuffer of size %d bytes\n",
                  framebufSize);
    HALT();
  } else {
    memset(framebuf, 0, framebufSize);
    logger.printf("Allocated framebuffer of size %d bytes\n", framebufSize);
  }

  gif.begin(LITTLE_ENDIAN_PIXELS);

  logger.println("Opening GIF file /fireplace.gif");
  if (gif.open("/fireplace.gif", GIFOpenFile, GIFCloseFile, GIFReadFile,
               GIFSeekFile, GIFDraw)) {
    GIFINFO gi;
    logger.printf("Successfully opened GIF\nCanvas size is %d by %d px\n",
                  gif.getCanvasWidth(), gif.getCanvasHeight());
    if (gif.getInfo(&gi)) {
      logger.printf("frame count: %d\n", gi.iFrameCount);
      logger.printf("duration: %d ms\n", gi.iDuration);
      logger.printf("max delay: %d ms\n", gi.iMaxDelay);
      logger.printf("min delay: %d ms\n", gi.iMinDelay);
    } else {
      logger.println("Failed to get GIF info");
      HALT();
    }
#ifdef DEBUG
    delay(1000);
#endif
    while (true) {
      uint8_t i = 0;
      uint32_t frameStart = millis();
      while (true) {
        int toDelay;
        tft.startWrite();
        int result = gif.playFrame(false, &toDelay);
        tft.endWrite();
        uint32_t frameDecodeEnd = millis();
        if (result == 0) {
          if (gif.getLastError() != GIF_SUCCESS &&
              gif.getLastError() != GIF_EMPTY_FRAME) {
            logger.printf("Error playing last frame = %d\n",
                          gif.getLastError());
            HALT();
          }
          break;
        } else if (result < 0) {
          logger.printf("Error playing frame = %d\n", gif.getLastError());
          HALT();
        }
#ifdef DEBUG
        tft.setCursor(0, 0);
        tft.printf("#%d %d ms    ", i++, frameDecodeEnd - frameStart);
#endif
        while (millis() - frameStart < toDelay) {
          yield();
        }
        frameStart = millis();
      }
      gif.reset();
    }
  } else {
    logger.printf("Error opening file = %d\n", gif.getLastError());
    HALT();
  }
}

void loop() {}

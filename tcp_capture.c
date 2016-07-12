#define OUTFILE_NAME	"capture.yuv"
#define COUNT_IGNORE	10	
#define IMAGE_WIDTH	640
#define IMAGE_HEIGHT	480

#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <asm/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>

#define CLEAR(x) memset (&(x), 0, sizeof (x))

#define MAXCONNECTION   5
#define BUFSIZE       153600

struct buffer {
  void *                  start;
  size_t                  length;
};

static char *           dev_name        = NULL;
static int              fd              = -1;
struct buffer *         buffers         = NULL;
static unsigned int     n_buffers       = 0;

static void errno_exit(const char *s)
{
  fprintf(stderr, "%s error %d, %s\n",s, errno, strerror(errno));  
  exit(EXIT_FAILURE);
}

static int xioctl(int fd,int request,void *arg)
{
  int r;
  do{ r = ioctl(fd, request, arg); }
  while(-1 == r && EINTR == errno);
  return r;
}

static void process_image(const void *p_buf,const int len_buf)
{
  FILE *fp;
  int i;
  char *d1,*d2,buf;

  printf("Capture size : %d\n",len_buf);

  // mmm... swap byte order
  // for(i=0;i<len_buf;i+=2)
    // {
      // d1 = (char *)(p_buf) + i;
      // d2 = (char *)(p_buf) + i + 1;
      // buf = *d1;
      // *d1 = *d2;
      // *d2 = buf;
    // }

  fp = fopen(OUTFILE_NAME,"wb");
  if(fp == NULL) return;
  fwrite(p_buf,len_buf,1,fp);
  fclose(fp);
  printf("Saved\n");
}

static int read_frame(int count)
{
  struct v4l2_buffer buf;

  CLEAR(buf);
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  if(-1 == xioctl(fd, VIDIOC_DQBUF, &buf))
      {
	switch(errno)
	    {
	    case EAGAIN:
	      return 0;
	    case EIO:
	      /* Could ignore EIO, see spec. */
	      /* fall through */
	    default:
	      errno_exit("VIDIOC_DQBUF");
	    }
      }
  assert(buf.index < n_buffers);
  if(count == 0)
    process_image(buffers[buf.index].start,buffers[buf.index].length);
  if(-1 == xioctl(fd, VIDIOC_QBUF, &buf))
    errno_exit("VIDIOC_QBUF");
  
  return 1;
}

static void mainloop(void)
{
  unsigned int count;
  count = COUNT_IGNORE;
  while(count-- > 0)
    {
      for(;;)
	{
	  fd_set fds;
	  struct timeval tv;
	  int r;
	  FD_ZERO(&fds);
	  FD_SET(fd, &fds);
	  /* Timeout. */
	  tv.tv_sec = 2;
	  tv.tv_usec = 0;
	  r = select(fd + 1, &fds, NULL, NULL, &tv);
	  if(-1 == r)
	    {
	      if(EINTR == errno)
		continue;
	      errno_exit("select");
	    }
	  if(0 == r)
	    {
	      fprintf(stderr, "select timeout\n");
	      exit(EXIT_FAILURE);
	    }
	  if(read_frame(count))
	    break;
	  /* EAGAIN - continue select loop. */
	}
    }
}

static void stop_capturing (void)
{
  enum v4l2_buf_type type;
  
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if(-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
    errno_exit("VIDIOC_STREAMOFF");
}

static void start_capturing(void)
{
  unsigned int i;
  enum v4l2_buf_type type;
  
  for(i = 0; i < n_buffers; ++i)
      {
	struct v4l2_buffer buf;
	CLEAR(buf);
	buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory      = V4L2_MEMORY_MMAP;
	buf.index       = i;
	
	if(-1 == xioctl(fd, VIDIOC_QBUF, &buf))
	  errno_exit("VIDIOC_QBUF");
      }
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if(-1 == xioctl(fd, VIDIOC_STREAMON, &type))
    errno_exit("VIDIOC_STREAMON");
}

static void uninit_device(void)
{
  unsigned int i;
  
  for(i = 0; i < n_buffers; ++i)
    if(-1 == munmap(buffers[i].start, buffers[i].length))
      errno_exit("munmap");
  free(buffers);
}

static void init_mmap(void)
{
  struct v4l2_requestbuffers req;
  
  CLEAR(req);
  
  req.count               = 8;
  req.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory              = V4L2_MEMORY_MMAP;
  
  if(-1 == xioctl(fd, VIDIOC_REQBUFS, &req))
    {
      if(EINVAL == errno)
	{
	  fprintf(stderr, "%s does not support "
		  "memory mapping\n", dev_name);
	  exit(EXIT_FAILURE);
	}
      else
	{
	  errno_exit("VIDIOC_REQBUFS");
	}
    }
  if(req.count < 2)
    {
      fprintf(stderr, "Insufficient buffer memory on %s\n",
	      dev_name);
      exit(EXIT_FAILURE);
    }
  buffers = calloc(req.count, sizeof(*buffers));
  
  if(!buffers)
    {
      fprintf(stderr, "Out of memory\n");
      exit(EXIT_FAILURE);
    }
  
  for(n_buffers = 0; n_buffers < req.count; ++n_buffers)
    {
      struct v4l2_buffer buf;
      CLEAR(buf);
      buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory      = V4L2_MEMORY_MMAP;
      buf.index       = n_buffers;
      
      if(-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
	errno_exit("VIDIOC_QUERYBUF");
  
      // printf("Buffer length : %d : %d\n",n_buffers,buf.length);
    
      buffers[n_buffers].length = buf.length;
      buffers[n_buffers].start =
	mmap(NULL /* start anywhere */,
	     buf.length,
	     PROT_READ | PROT_WRITE /* required */,
	     MAP_SHARED /* recommended */,
	     fd, buf.m.offset);
      
      if(MAP_FAILED == buffers[n_buffers].start)
	errno_exit("mmap");
    }
}

static void init_device(void)
{
  struct v4l2_capability cap;
  struct v4l2_cropcap cropcap;
  struct v4l2_crop crop;
  struct v4l2_format fmt;
  unsigned int min;
  
  if(-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap))
    {
      if(EINVAL == errno)
	{
	  fprintf(stderr, "%s is no V4L2 device\n",dev_name);
	  exit(EXIT_FAILURE);
	}
      else
	{
	  errno_exit("VIDIOC_QUERYCAP");
	}
    }
  if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
      fprintf(stderr, "%s is no video capture device\n",dev_name);
      exit(EXIT_FAILURE);
    }
  if(!(cap.capabilities & V4L2_CAP_STREAMING))
      {
	fprintf(stderr, "%s does not support streaming i/o\n",
		dev_name);
	exit(EXIT_FAILURE);
      }
  /* Select video input, video standard and tune here. */
  CLEAR(cropcap);
  
  cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  
  if(0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap))
    {
      crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      crop.c = cropcap.defrect; /* reset to default */
      
      if(-1 == xioctl(fd, VIDIOC_S_CROP, &crop))
	{
	  switch(errno)
	    {
	    case EINVAL:
	      /* Cropping not supported. */
	      break;
	    default:
	      /* Errors ignored. */
	      break;
	    }
	}
    }
  else
    {	
      /* Errors ignored. */
    }
  CLEAR(fmt);
  
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = IMAGE_WIDTH;
  fmt.fmt.pix.height      = IMAGE_HEIGHT;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;
  
  if(-1 == xioctl(fd, VIDIOC_S_FMT, &fmt)) errno_exit("VIDIOC_S_FMT");
  
  /* Note VIDIOC_S_FMT may change width and height. */
  
  /* Buggy driver paranoia. */
  min = fmt.fmt.pix.width * 2;
  if(fmt.fmt.pix.bytesperline < min) fmt.fmt.pix.bytesperline = min;
  min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
  if(fmt.fmt.pix.sizeimage < min) fmt.fmt.pix.sizeimage = min;
  
  init_mmap();
}

static void close_device(void)
{
  if(-1 == close(fd)) errno_exit("close");
  fd = -1;
}

static void open_device(void)
{
  struct stat st; 
  
  if(-1 == stat(dev_name, &st))
    {
      fprintf(stderr, "Cannot identify '%s': %d, %s\n",
	      dev_name, errno, strerror(errno));
      exit(EXIT_FAILURE);
    }
  
  if(!S_ISCHR(st.st_mode))
    {
      fprintf(stderr, "%s is no device\n", dev_name);
      exit(EXIT_FAILURE);
    }
  fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);
  if(-1 == fd)
    {
      fprintf(stderr, "Cannot open '%s': %d, %s\n",
	      dev_name, errno, strerror(errno));
      exit(EXIT_FAILURE);
    }
}


static void session_with_client (int);

int main(void)
{

  int ssid, csid, i;
  struct sockaddr_in s_addr, c_addr;

  // if (argc < 2)
  // {
    // fprintf (stderr, "Usage: %s PORT\n", 9999);
    // exit (1);
  // }

  /* 소켓 작성과 데이터 준비 */
  if ((ssid = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
  {
    perror ("Socket creation failed");
    exit (1);
  }
  memset (&s_addr, 0, sizeof (s_addr));
  s_addr.sin_family = AF_INET;
  s_addr.sin_addr.s_addr = htonl (INADDR_ANY);
  s_addr.sin_port = htons (9999);

  /* 포트 할당과 커넥션 접수 */
  if (bind (ssid, (struct sockaddr *) &s_addr, sizeof (s_addr)) < 0)
  {
    perror ("Binding socket failed");
    exit (1);
  }
  if (listen (ssid, MAXCONNECTION) < 0)
  {
    perror ("Listening server socket failed");
    exit (1);
  }



  /* 클라이언트와의 세션 */
  while (1)
  {
    unsigned int len = sizeof (c_addr);

    /* 클라이언트로부터의 커넥션 대기 */
    if ((csid = accept (ssid, (struct sockaddr *) &c_addr, &len)) < 0)
    {
      perror ("Client connection failed");
      exit (1);
    }
	
    printf ("Client connected: %s\n", inet_ntoa (c_addr.sin_addr));
	
	dev_name = "/dev/video0";
  
  open_device();
  init_device();
  start_capturing();
  mainloop();
  stop_capturing();
  uninit_device();
  close_device();
  // exit(EXIT_SUCCESS);
  
  
	system("ffmpeg -s 320x240 -pix_fmt yuyv422 -i capture.yuv capture.jpg -y");
	
	

    /* 세션 시작 */
    session_with_client (csid);
	printf("Transmission complete");
  }
}

void session_with_client (int sid)
{
  char buffer[BUFSIZE];
  int sended = -1;
  int fd1;

  if((fd1 = open("./capture.jpg",O_RDWR|O_EXCL, 0777))==-1)
  {
    perror("open failed");
    exit(1);
  }

  read(fd1, buffer, BUFSIZE);
  
  switch(sended = send (sid, buffer, sizeof(buffer), 0))
  {
	case -1: 
		perror ("Error in sending message to client");
		exit (1);
		break;
	case 153600: 
		break;
	
	
  }
  

  printf("%d", sended);
  close(fd1);
  close (sid);
}

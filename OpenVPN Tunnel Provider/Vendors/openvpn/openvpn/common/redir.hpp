//    OpenVPN -- An application to securely tunnel IP networks
//               over a single port, with support for SSL/TLS-based
//               session authentication and key exchange,
//               packet encryption, packet authentication, and
//               packet compression.
//
//    Copyright (C) 2012-2016 OpenVPN Technologies, Inc.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License Version 3
//    as published by the Free Software Foundation.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program in the COPYING file.
//    If not, see <http://www.gnu.org/licenses/>.

#ifndef OPENVPN_COMMON_REDIR_H
#define OPENVPN_COMMON_REDIR_H

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <string>
#include <utility>
#include <memory>
#include <algorithm>

#include <asio.hpp>

#include <openvpn/common/exception.hpp>
#include <openvpn/common/scoped_fd.hpp>
#include <openvpn/common/tempfile.hpp>
#include <openvpn/buffer/buflist.hpp>

namespace openvpn {

  struct RedirectBase
  {
    OPENVPN_EXCEPTION(redirect_std_err);
    virtual void redirect() = 0;
    virtual void close() = 0;
    virtual ~RedirectBase() {}
  };

  struct RedirectStdFD : public RedirectBase
  {
    virtual void redirect() noexcept override
    {
      // stdin
      if (in.defined())
	{
	  ::dup2(in(), 0);
	  if (in() <= 2)
	    in.release();
	}

      // stdout
      if (out.defined())
	{
	  ::dup2(out(), 1);
	  if (!err.defined() && combine_out_err)
	    ::dup2(out(), 2);
	  if (out() <= 2)
	    out.release();
	}

      // stderr
      if (err.defined())
	{
	  ::dup2(err(), 2);
	  if (err() <= 2)
	    err.release();
	}

      close();
    }

    virtual void close() override
    {
      in.close();
      out.close();
      err.close();
    }

    ScopedFD in;
    ScopedFD out;
    ScopedFD err;
    bool combine_out_err = false;
  };

  class RedirectStd : public RedirectStdFD
  {
  public:
    // flags shortcuts
    static constexpr int FLAGS_OVERWRITE = O_CREAT | O_WRONLY | O_TRUNC;
    static constexpr int FLAGS_APPEND = O_CREAT | O_WRONLY | O_APPEND;
    static constexpr int FLAGS_MUST_NOT_EXIST = O_CREAT | O_WRONLY | O_EXCL;

    // mode shortcuts
    static constexpr mode_t MODE_ALL = 0777;
    static constexpr mode_t MODE_USER_GROUP = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
    static constexpr mode_t MODE_USER = S_IRUSR | S_IWUSR;

    RedirectStd(const std::string& in_fn,
		const std::string& out_fn,
		const int out_flags = FLAGS_OVERWRITE,
		const mode_t out_mode = MODE_ALL,
		const bool combine_out_err_arg = true)
    {
      if (!in_fn.empty())
	open_input(in_fn);
      open_output(out_fn, out_flags, out_mode);
      combine_out_err = combine_out_err_arg;
    }

  protected:
    RedirectStd() {}

    void open_input(const std::string& fn)
    {
      // open input file for stdin
      in.reset(::open(fn.c_str(), O_RDONLY, 0));
      if (!in.defined())
	{
	  const int eno = errno;
	  OPENVPN_THROW(redirect_std_err, "error opening input file: " << fn << " : " << std::strerror(eno));
	}
    }

    void open_output(const std::string& fn,
		     const int flags,
		     const mode_t mode)
    {
      // open output file for stdout/stderr
      out.reset(::open(fn.c_str(),
		       flags,
		       mode));
      if (!out.defined())
	{
	  const int eno = errno;
	  OPENVPN_THROW(redirect_std_err, "error opening output file: " << fn << " : " << std::strerror(eno));
	}
    }
  };

  class RedirectTemp : public RedirectStd
  {
  public:
    RedirectTemp(const std::string& stdin_fn,
		 TempFile& stdout_temp,
		 const bool combine_out_err_arg)
    {
      open_input(stdin_fn);
      out = std::move(stdout_temp.fd);
      combine_out_err = combine_out_err_arg;
    }

    RedirectTemp(const std::string& stdin_fn,
		 TempFile& stdout_temp,
		 TempFile& stderr_temp)
    {
      open_input(stdin_fn);
      out = std::move(stdout_temp.fd);
      err = std::move(stderr_temp.fd);
    }
  };

  class RedirectPipe : public RedirectStdFD
  {
  public:
    struct InOut
    {
      std::string in;
      std::string out;
      std::string err;
    };

    RedirectPipe() {}

    RedirectPipe(RedirectStdFD& remote,
		 const bool combine_out_err_arg,
		 const bool enable_in)
    {
      int fd[2];

      // stdout
      make_pipe(fd);
      out.reset(cloexec(fd[0]));
      remote.out.reset(fd[1]);

      // stderr
      combine_out_err = remote.combine_out_err = combine_out_err_arg;
      if (!combine_out_err)
	{
	  make_pipe(fd);
	  err.reset(cloexec(fd[0]));
	  remote.err.reset(fd[1]);
	}

      // stdin
      if (enable_in)
	{
	  make_pipe(fd);
	  in.reset(cloexec(fd[1]));
	  remote.in.reset(fd[0]);
	}
      else
	{
	  // open /dev/null for stdin
	  remote.in.reset(::open("/dev/null", O_RDONLY, 0));
	  if (!remote.in.defined())
	    {
	      const int eno = errno;
	      OPENVPN_THROW(redirect_std_err, "error opening /dev/null : " << std::strerror(eno));
	    }
	}
    }

    void transact(InOut& inout)
    {
      asio::io_context io_context(1);
      SD_OUT send_in(io_context, inout.in, in);
      SD_IN recv_out(io_context, out);
      SD_IN recv_err(io_context, err);
      io_context.run();
      inout.out = recv_out.content();
      inout.err = recv_err.content();
    }

  private:
    class SD
    {
    public:
      SD(asio::io_context& io_context, ScopedFD& fd)
      {
	if (fd.defined())
	  sd.reset(new asio::posix::stream_descriptor(io_context, fd.release()));
      }

      bool defined() const
      {
	return bool(sd);
      }

    protected:
      std::unique_ptr<asio::posix::stream_descriptor> sd;
    };

    class SD_OUT : public SD
    {
    public:
      SD_OUT(asio::io_context& io_context, const std::string& content, ScopedFD& fd)
	: SD(io_context, fd)
      {
	if (defined())
	  {
	    buf = buf_alloc_from_string(content);
	    queue_write();
	  }
      }

    private:
      void queue_write()
      {
	sd->async_write_some(buf.const_buffers_1_limit(2048),
			     [this](const asio::error_code& ec, const size_t bytes_sent) {
			       if (!ec && bytes_sent < buf.size())
				 {
				   buf.advance(bytes_sent);
				   queue_write();
				 }
			       else
				 sd->close();
			     });
      }

      BufferAllocated buf;
    };

    class SD_IN : public SD
    {
    public:
      SD_IN(asio::io_context& io_context, ScopedFD& fd)
	: SD(io_context, fd)
      {
	if (defined())
	  queue_read();
      }

      const std::string content() const
      {
	return data.to_string();
      }

    private:
      void queue_read()
      {
	buf.reset(0, 2048, 0);
	sd->async_read_some(buf.mutable_buffers_1_clamp(),
			    [this](const asio::error_code& ec, const size_t bytes_recvd) {
			      if (!ec)
				{
				  buf.set_size(bytes_recvd);
				  data.put_consume(buf);
				  queue_read();
				}
			      else
				sd->close();
			    });
      }

      BufferAllocated buf;
      BufferList data;
    };

    static void make_pipe(int fd[2])
    {
      if (::pipe(fd) < 0)
	{
	  const int eno = errno;
	  OPENVPN_THROW(redirect_std_err, "error creating pipe : " << std::strerror(eno));
	}
    }

    // set FD_CLOEXEC to prevent fd from being passed across execs
    static int cloexec(const int fd)
    {
      if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
	{
	  const int eno = errno;
	  OPENVPN_THROW(redirect_std_err, "error setting FD_CLOEXEC on pipe : " << std::strerror(eno));
	}
      return fd;
    }

  };
}

#endif

package ProFTPD::TestSuite::FTP;

use strict;

use Carp;
use Net::FTP;
use POSIX qw(:sys_wait_h);

sub new {
  my $class = shift;
  my ($addr, $port, $timeout) = @_;
  $timeout = 10 unless defined($timeout);
 
  my $ftp;

  my $now = time();

  # Creating a Net::FTP object involves attempting to connect to the given
  # address/port.  So handle the test cases where the server process may
  # not yet be completely up, retry this connect, once a second, up to the
  # given timeout.

  while (1) {
    if (time() - $now > $timeout) {
      croak("Unable to connect to $addr:$port: Timed out after $timeout secs");
    }

    $ftp = Net::FTP->new(
      Host => $addr,
      Port => $port,
    );

    last if $ftp;
    sleep(1);
  }

  my $self = {
    addr => $addr,
    ftp => $ftp,
    port => $port,
  };

  bless($self, $class);
  return $self;
}

sub response_code {
  my $self = shift;
  return $self->{ftp}->code;
}

sub response_msg {
  my $self = shift;
  return $self->{ftp}->message;
}

sub login {
  my $self = shift;
  my $user = shift;
  croak("Missing required user argument") unless $user;
  my $pass = shift;
  croak("Missing required password argument") unless $pass;

  unless ($self->{ftp}->login($user, $pass)) {
    croak("Failed to login to $self->{addr}:$self->{port}: " .
      $self->{ftp}->code . ' ' . $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub pwd {
  my $self = shift;

  unless ($self->{ftp}->pwd()) {
    croak("PWD command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub xpwd {
  my $self = shift;

  unless ($self->{ftp}->quot('XPWD')) {
    croak("XPWD command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub cwd {
  my $self = shift;
  my $dir = shift;

  unless ($self->{ftp}->cwd($dir)) {
    croak("CWD command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub xcwd {
  my $self = shift;
  my $dir = shift;

  unless ($self->{ftp}->quot('XCWD', $dir)) {
    croak("XCWD command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub cdup {
  my $self = shift;
  my $dir = shift;

  unless ($self->{ftp}->cdup()) {
    croak("CDUP command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub xcup {
  my $self = shift;

  unless ($self->{ftp}->quot('XCUP')) {
    croak("XCUP command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub syst {
  my $self = shift;

  unless ($self->{ftp}->quot('SYST')) {
    croak("SYST command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub mkd {
  my $self = shift;
  my $dir = shift;

  unless ($self->{ftp}->mkdir($dir)) {
    croak("MKD command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub xmkd {
  my $self = shift;
  my $dir = shift;

  unless ($self->{ftp}->quot('XMKD', $dir)) {
    croak("XMKD command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub rmd {
  my $self = shift;
  my $dir = shift;

  unless ($self->{ftp}->rmdir($dir)) {
    croak("RMD command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

sub xrmd {
  my $self = shift;
  my $dir = shift;

  unless ($self->{ftp}->quot('XRMD', $dir)) {
    croak("XRMD command failed: " .  $self->{ftp}->code . ' ' .
      $self->{ftp}->message);
  }

  if (wantarray()) {
    return ($self->{ftp}->code, $self->{ftp}->message);

  } else {
    return $self->{ftp}->message;
  }
}

1;

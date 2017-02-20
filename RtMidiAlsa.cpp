/**********************************************************************/
/*! \class RtMidi
    \brief An abstract base class for realtime MIDI input/output.

    This class implements some common functionality for the realtime
    MIDI input/output subclasses RtMidiIn and RtMidiOut.

    RtMidi WWW site: http://music.mcgill.ca/~gary/rtmidi/

    RtMidi: realtime MIDI i/o C++ classes
    Copyright (c) 2003-2016 Gary P. Scavone

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation files
    (the "Software"), to deal in the Software without restriction,
    including without limitation the rights to use, copy, modify, merge,
    publish, distribute, sublicense, and/or sell copies of the Software,
    and to permit persons to whom the Software is furnished to do so,
    subject to the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    Any person wishing to distribute modifications to the Software is
    asked to send the modifications to the original developer so that
    they can be incorporated into the canonical version.  This is,
    however, not a binding provision of this license.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
    ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
    CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
    WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
/**********************************************************************/

#include "RtMidi.h"
#include "RtMidiAlsa.h"

#include <sstream>

//*********************************************************************//
//  API: LINUX ALSA SEQUENCER
//*********************************************************************//

// API information found at:
//   - http://www.alsa-project.org/documentation.php#Library

#if defined(__LINUX_ALSA__)

// The ALSA Sequencer API is based on the use of a callback function for
// MIDI input.
//
// Thanks to Pedro Lopez-Cabanillas for help with the ALSA sequencer
// time stamps and other assorted fixes!!!

// If you don't need timestamping for incoming MIDI events, define the
// preprocessor definition AVOID_TIMESTAMPING to save resources
// associated with the ALSA sequencer queues.

#include <pthread.h>
#include <sys/time.h>

// ALSA header file.
#include <alsa/asoundlib.h>

// A structure to hold variables related to the ALSA API
// implementation.
struct AlsaMidiData {
  snd_seq_t *seq;
  unsigned int portNum;
  int vport;
  snd_seq_port_subscribe_t *subscription;
  snd_midi_event_t *coder;
  unsigned int bufferSize;
  unsigned char *buffer;
  pthread_t thread;
  pthread_t dummy_thread_id;
  unsigned long long lastTime;
  int queue_id; // an input queue is needed to get timestamped events
  int trigger_fds[2];
};

#define PORT_TYPE( pinfo, bits ) ((snd_seq_port_info_get_capability(pinfo) & (bits)) == (bits))

//*********************************************************************//
//  API: LINUX ALSA
//  Class Definitions: MidiInAlsa
//*********************************************************************//

static void *alsaMidiHandler( void *ptr )
{
  MidiInApi::RtMidiInData *data = static_cast<MidiInApi::RtMidiInData *> (ptr);
  AlsaMidiData *apiData = static_cast<AlsaMidiData *> (data->apiData);

  long nBytes;
  unsigned long long time, lastTime;
  bool continueSysex = false;
  bool doDecode = false;
  MidiInApi::MidiMessage message;
  int poll_fd_count;
  struct pollfd *poll_fds;

  snd_seq_event_t *ev;
  int result;
  apiData->bufferSize = 32;
  result = snd_midi_event_new( 0, &apiData->coder );
  if ( result < 0 ) {
    data->doInput = false;
    std::cerr << "\nMidiInAlsa::alsaMidiHandler: error initializing MIDI event parser!\n\n";
    return 0;
  }
  unsigned char *buffer = (unsigned char *) malloc( apiData->bufferSize );
  if ( buffer == NULL ) {
    data->doInput = false;
    snd_midi_event_free( apiData->coder );
    apiData->coder = 0;
    std::cerr << "\nMidiInAlsa::alsaMidiHandler: error initializing buffer memory!\n\n";
    return 0;
  }
  snd_midi_event_init( apiData->coder );
  snd_midi_event_no_status( apiData->coder, 1 ); // suppress running status messages

  poll_fd_count = snd_seq_poll_descriptors_count( apiData->seq, POLLIN ) + 1;
  poll_fds = (struct pollfd*)alloca( poll_fd_count * sizeof( struct pollfd ));
  snd_seq_poll_descriptors( apiData->seq, poll_fds + 1, poll_fd_count - 1, POLLIN );
  poll_fds[0].fd = apiData->trigger_fds[0];
  poll_fds[0].events = POLLIN;

  while ( data->doInput ) {

    if ( snd_seq_event_input_pending( apiData->seq, 1 ) == 0 ) {
      // No data pending
      if ( poll( poll_fds, poll_fd_count, -1) >= 0 ) {
        if ( poll_fds[0].revents & POLLIN ) {
          bool dummy;
          int res = read( poll_fds[0].fd, &dummy, sizeof(dummy) );
          (void) res;
        }
      }
      continue;
    }

    // If here, there should be data.
    result = snd_seq_event_input( apiData->seq, &ev );
    if ( result == -ENOSPC ) {
      std::cerr << "\nMidiInAlsa::alsaMidiHandler: MIDI input buffer overrun!\n\n";
      continue;
    }
    else if ( result <= 0 ) {
      std::cerr << "\nMidiInAlsa::alsaMidiHandler: unknown MIDI input error!\n";
      perror("System reports");
      continue;
    }

    // This is a bit weird, but we now have to decode an ALSA MIDI
    // event (back) into MIDI bytes.  We'll ignore non-MIDI types.
    if ( !continueSysex ) message.bytes.clear();

    doDecode = false;
    switch ( ev->type ) {

    case SND_SEQ_EVENT_PORT_SUBSCRIBED:
#if defined(__RTMIDI_DEBUG__)
      std::cout << "MidiInAlsa::alsaMidiHandler: port connection made!\n";
#endif
      break;

    case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
#if defined(__RTMIDI_DEBUG__)
      std::cerr << "MidiInAlsa::alsaMidiHandler: port connection has closed!\n";
      std::cout << "sender = " << (int) ev->data.connect.sender.client << ":"
                << (int) ev->data.connect.sender.port
                << ", dest = " << (int) ev->data.connect.dest.client << ":"
                << (int) ev->data.connect.dest.port
                << std::endl;
#endif
      break;

    case SND_SEQ_EVENT_QFRAME: // MIDI time code
      if ( !( data->ignoreFlags & 0x02 ) ) doDecode = true;
      break;

    case SND_SEQ_EVENT_TICK: // 0xF9 ... MIDI timing tick
      if ( !( data->ignoreFlags & 0x02 ) ) doDecode = true;
      break;

    case SND_SEQ_EVENT_CLOCK: // 0xF8 ... MIDI timing (clock) tick
      if ( !( data->ignoreFlags & 0x02 ) ) doDecode = true;
      break;

    case SND_SEQ_EVENT_SENSING: // Active sensing
      if ( !( data->ignoreFlags & 0x04 ) ) doDecode = true;
      break;

		case SND_SEQ_EVENT_SYSEX:
      if ( (data->ignoreFlags & 0x01) ) break;
      if ( ev->data.ext.len > apiData->bufferSize ) {
        apiData->bufferSize = ev->data.ext.len;
        free( buffer );
        buffer = (unsigned char *) malloc( apiData->bufferSize );
        if ( buffer == NULL ) {
          data->doInput = false;
          std::cerr << "\nMidiInAlsa::alsaMidiHandler: error resizing buffer memory!\n\n";
          break;
        }
      }

    default:
      doDecode = true;
    }

    if ( doDecode ) {

      nBytes = snd_midi_event_decode( apiData->coder, buffer, apiData->bufferSize, ev );
      if ( nBytes > 0 ) {
        // The ALSA sequencer has a maximum buffer size for MIDI sysex
        // events of 256 bytes.  If a device sends sysex messages larger
        // than this, they are segmented into 256 byte chunks.  So,
        // we'll watch for this and concatenate sysex chunks into a
        // single sysex message if necessary.
        if ( !continueSysex )
          message.bytes.assign( buffer, &buffer[nBytes] );
        else
          message.bytes.insert( message.bytes.end(), buffer, &buffer[nBytes] );

        continueSysex = ( ( ev->type == SND_SEQ_EVENT_SYSEX ) && ( message.bytes.back() != 0xF7 ) );
        if ( !continueSysex ) {

          // Calculate the time stamp:
          message.timeStamp = 0.0;

          // Method 1: Use the system time.
          //(void)gettimeofday(&tv, (struct timezone *)NULL);
          //time = (tv.tv_sec * 1000000) + tv.tv_usec;

          // Method 2: Use the ALSA sequencer event time data.
          // (thanks to Pedro Lopez-Cabanillas!).
          time = ( ev->time.time.tv_sec * 1000000 ) + ( ev->time.time.tv_nsec/1000 );
          lastTime = time;
          time -= apiData->lastTime;
          apiData->lastTime = lastTime;
          if ( data->firstMessage == true )
            data->firstMessage = false;
          else
            message.timeStamp = time * 0.000001;
        }
        else {
#if defined(__RTMIDI_DEBUG__)
          std::cerr << "\nMidiInAlsa::alsaMidiHandler: event parsing error or not a MIDI event!\n\n";
#endif
        }
      }
    }

    snd_seq_free_event( ev );
    if ( message.bytes.size() == 0 || continueSysex ) continue;

    if ( data->usingCallback ) {
      RtMidiIn::RtMidiCallback callback = (RtMidiIn::RtMidiCallback) data->userCallback;
      callback( message.timeStamp, &message.bytes, data->userData );
    }
    else {
      // As long as we haven't reached our queue size limit, push the message.
      if ( data->queue.size < data->queue.ringSize ) {
        data->queue.ring[data->queue.back++] = message;
        if ( data->queue.back == data->queue.ringSize )
          data->queue.back = 0;
        data->queue.size++;
      }
      else
        std::cerr << "\nMidiInAlsa: message queue limit reached!!\n\n";
    }
  }

  if ( buffer ) free( buffer );
  snd_midi_event_free( apiData->coder );
  apiData->coder = 0;
  apiData->thread = apiData->dummy_thread_id;
  return 0;
}

MidiInAlsa :: MidiInAlsa( const std::string clientName, unsigned int queueSizeLimit ) : MidiInApi( queueSizeLimit )
{
  initialize( clientName );
}

MidiInAlsa :: ~MidiInAlsa()
{
  // Close a connection if it exists.
  closePort();

  // Shutdown the input thread.
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  if ( inputData_.doInput ) {
    inputData_.doInput = false;
    int res = write( data->trigger_fds[1], &inputData_.doInput, sizeof(inputData_.doInput) );
    (void) res;
    if ( !pthread_equal(data->thread, data->dummy_thread_id) )
      pthread_join( data->thread, NULL );
  }

  // Cleanup.
  close ( data->trigger_fds[0] );
  close ( data->trigger_fds[1] );
  if ( data->vport >= 0 ) snd_seq_delete_port( data->seq, data->vport );
#ifndef AVOID_TIMESTAMPING
  snd_seq_free_queue( data->seq, data->queue_id );
#endif
  snd_seq_close( data->seq );
  delete data;
}

void MidiInAlsa :: initialize( const std::string& clientName )
{
  // Set up the ALSA sequencer client.
  snd_seq_t *seq;
  int result = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
  if ( result < 0 ) {
    errorString_ = "MidiInAlsa::initialize: error creating ALSA sequencer client object.";
    error( RtMidiError::DRIVER_ERROR, errorString_ );
    return;
  }

  // Set client name.
  snd_seq_set_client_name( seq, clientName.c_str() );

  // Save our api-specific connection information.
  AlsaMidiData *data = (AlsaMidiData *) new AlsaMidiData;
  data->seq = seq;
  data->portNum = -1;
  data->vport = -1;
  data->subscription = 0;
  data->dummy_thread_id = pthread_self();
  data->thread = data->dummy_thread_id;
  data->trigger_fds[0] = -1;
  data->trigger_fds[1] = -1;
  apiData_ = (void *) data;
  inputData_.apiData = (void *) data;

   if ( pipe(data->trigger_fds) == -1 ) {
    errorString_ = "MidiInAlsa::initialize: error creating pipe objects.";
    error( RtMidiError::DRIVER_ERROR, errorString_ );
    return;
  }

  // Create the input queue
#ifndef AVOID_TIMESTAMPING
  data->queue_id = snd_seq_alloc_named_queue(seq, "RtMidi Queue");
  // Set arbitrary tempo (mm=100) and resolution (240)
  snd_seq_queue_tempo_t *qtempo;
  snd_seq_queue_tempo_alloca(&qtempo);
  snd_seq_queue_tempo_set_tempo(qtempo, 600000);
  snd_seq_queue_tempo_set_ppq(qtempo, 240);
  snd_seq_set_queue_tempo(data->seq, data->queue_id, qtempo);
  snd_seq_drain_output(data->seq);
#endif
}

// This function is used to count or get the pinfo structure for a given port number.
unsigned int portInfo( snd_seq_t *seq, snd_seq_port_info_t *pinfo, unsigned int type, int portNumber )
{
  snd_seq_client_info_t *cinfo;
  int client;
  int count = 0;
  snd_seq_client_info_alloca( &cinfo );

  snd_seq_client_info_set_client( cinfo, -1 );
  while ( snd_seq_query_next_client( seq, cinfo ) >= 0 ) {
    client = snd_seq_client_info_get_client( cinfo );
    if ( client == 0 ) continue;
    // Reset query info
    snd_seq_port_info_set_client( pinfo, client );
    snd_seq_port_info_set_port( pinfo, -1 );
    while ( snd_seq_query_next_port( seq, pinfo ) >= 0 ) {
      unsigned int atyp = snd_seq_port_info_get_type( pinfo );
      if ( ( ( atyp & SND_SEQ_PORT_TYPE_MIDI_GENERIC ) == 0 ) &&
        ( ( atyp & SND_SEQ_PORT_TYPE_SYNTH ) == 0 ) ) continue;
      unsigned int caps = snd_seq_port_info_get_capability( pinfo );
      if ( ( caps & type ) != type ) continue;
      if ( count == portNumber ) return 1;
      ++count;
    }
  }

  // If a negative portNumber was used, return the port count.
  if ( portNumber < 0 ) return count;
  return 0;
}

unsigned int MidiInAlsa :: getPortCount()
{
  snd_seq_port_info_t *pinfo;
  snd_seq_port_info_alloca( &pinfo );

  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  return portInfo( data->seq, pinfo, SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ, -1 );
}

std::string MidiInAlsa :: getPortName( unsigned int portNumber )
{
  snd_seq_client_info_t *cinfo;
  snd_seq_port_info_t *pinfo;
  snd_seq_client_info_alloca( &cinfo );
  snd_seq_port_info_alloca( &pinfo );

  std::string stringName;
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  if ( portInfo( data->seq, pinfo, SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ, (int) portNumber ) ) {
    int cnum = snd_seq_port_info_get_client( pinfo );
    snd_seq_get_any_client_info( data->seq, cnum, cinfo );
    std::ostringstream os;
    os << snd_seq_client_info_get_name( cinfo );
    os << " ";                                    // These lines added to make sure devices are listed
    os << snd_seq_port_info_get_client( pinfo );  // with full portnames added to ensure individual device names
    os << ":";
    os << snd_seq_port_info_get_port( pinfo );
    stringName = os.str();
    return stringName;
  }

  // If we get here, we didn't find a match.
  errorString_ = "MidiInAlsa::getPortName: error looking for port name!";
  error( RtMidiError::WARNING, errorString_ );
  return stringName;
}

void MidiInAlsa :: openPort( unsigned int portNumber, const std::string portName )
{
  if ( connected_ ) {
    errorString_ = "MidiInAlsa::openPort: a valid connection already exists!";
    error( RtMidiError::WARNING, errorString_ );
    return;
  }

  unsigned int nSrc = this->getPortCount();
  if ( nSrc < 1 ) {
    errorString_ = "MidiInAlsa::openPort: no MIDI input sources found!";
    error( RtMidiError::NO_DEVICES_FOUND, errorString_ );
    return;
  }

  snd_seq_port_info_t *src_pinfo;
  snd_seq_port_info_alloca( &src_pinfo );
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  if ( portInfo( data->seq, src_pinfo, SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ, (int) portNumber ) == 0 ) {
    std::ostringstream ost;
    ost << "MidiInAlsa::openPort: the 'portNumber' argument (" << portNumber << ") is invalid.";
    errorString_ = ost.str();
    error( RtMidiError::INVALID_PARAMETER, errorString_ );
    return;
  }

  snd_seq_addr_t sender, receiver;
  sender.client = snd_seq_port_info_get_client( src_pinfo );
  sender.port = snd_seq_port_info_get_port( src_pinfo );
  receiver.client = snd_seq_client_id( data->seq );

  snd_seq_port_info_t *pinfo;
  snd_seq_port_info_alloca( &pinfo );
  if ( data->vport < 0 ) {
    snd_seq_port_info_set_client( pinfo, 0 );
    snd_seq_port_info_set_port( pinfo, 0 );
    snd_seq_port_info_set_capability( pinfo,
                                      SND_SEQ_PORT_CAP_WRITE |
                                      SND_SEQ_PORT_CAP_SUBS_WRITE );
    snd_seq_port_info_set_type( pinfo,
                                SND_SEQ_PORT_TYPE_MIDI_GENERIC |
                                SND_SEQ_PORT_TYPE_APPLICATION );
    snd_seq_port_info_set_midi_channels(pinfo, 16);
#ifndef AVOID_TIMESTAMPING
    snd_seq_port_info_set_timestamping(pinfo, 1);
    snd_seq_port_info_set_timestamp_real(pinfo, 1);    
    snd_seq_port_info_set_timestamp_queue(pinfo, data->queue_id);
#endif
    snd_seq_port_info_set_name(pinfo,  portName.c_str() );
    data->vport = snd_seq_create_port(data->seq, pinfo);
  
    if ( data->vport < 0 ) {
      errorString_ = "MidiInAlsa::openPort: ALSA error creating input port.";
      error( RtMidiError::DRIVER_ERROR, errorString_ );
      return;
    }
    data->vport = snd_seq_port_info_get_port(pinfo);
  }

  receiver.port = data->vport;

  if ( !data->subscription ) {
    // Make subscription
    if (snd_seq_port_subscribe_malloc( &data->subscription ) < 0) {
      errorString_ = "MidiInAlsa::openPort: ALSA error allocation port subscription.";
      error( RtMidiError::DRIVER_ERROR, errorString_ );
      return;
    }
    snd_seq_port_subscribe_set_sender(data->subscription, &sender);
    snd_seq_port_subscribe_set_dest(data->subscription, &receiver);
    if ( snd_seq_subscribe_port(data->seq, data->subscription) ) {
      snd_seq_port_subscribe_free( data->subscription );
      data->subscription = 0;
      errorString_ = "MidiInAlsa::openPort: ALSA error making port connection.";
      error( RtMidiError::DRIVER_ERROR, errorString_ );
      return;
    }
  }

  if ( inputData_.doInput == false ) {
    // Start the input queue
#ifndef AVOID_TIMESTAMPING
    snd_seq_start_queue( data->seq, data->queue_id, NULL );
    snd_seq_drain_output( data->seq );
#endif
    // Start our MIDI input thread.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);

    inputData_.doInput = true;
    int err = pthread_create(&data->thread, &attr, alsaMidiHandler, &inputData_);
    pthread_attr_destroy(&attr);
    if ( err ) {
      snd_seq_unsubscribe_port( data->seq, data->subscription );
      snd_seq_port_subscribe_free( data->subscription );
      data->subscription = 0;
      inputData_.doInput = false;
      errorString_ = "MidiInAlsa::openPort: error starting MIDI input thread!";
      error( RtMidiError::THREAD_ERROR, errorString_ );
      return;
    }
  }

  connected_ = true;
}

void MidiInAlsa :: openVirtualPort( std::string portName )
{
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  if ( data->vport < 0 ) {
    snd_seq_port_info_t *pinfo;
    snd_seq_port_info_alloca( &pinfo );
    snd_seq_port_info_set_capability( pinfo,
				      SND_SEQ_PORT_CAP_WRITE |
				      SND_SEQ_PORT_CAP_SUBS_WRITE );
    snd_seq_port_info_set_type( pinfo,
				SND_SEQ_PORT_TYPE_MIDI_GENERIC |
				SND_SEQ_PORT_TYPE_APPLICATION );
    snd_seq_port_info_set_midi_channels(pinfo, 16);
#ifndef AVOID_TIMESTAMPING
    snd_seq_port_info_set_timestamping(pinfo, 1);
    snd_seq_port_info_set_timestamp_real(pinfo, 1);    
    snd_seq_port_info_set_timestamp_queue(pinfo, data->queue_id);
#endif
    snd_seq_port_info_set_name(pinfo, portName.c_str());
    data->vport = snd_seq_create_port(data->seq, pinfo);

    if ( data->vport < 0 ) {
      errorString_ = "MidiInAlsa::openVirtualPort: ALSA error creating virtual port.";
      error( RtMidiError::DRIVER_ERROR, errorString_ );
      return;
    }
    data->vport = snd_seq_port_info_get_port(pinfo);
  }

  if ( inputData_.doInput == false ) {
    // Wait for old thread to stop, if still running
    if ( !pthread_equal(data->thread, data->dummy_thread_id) )
      pthread_join( data->thread, NULL );

    // Start the input queue
#ifndef AVOID_TIMESTAMPING
    snd_seq_start_queue( data->seq, data->queue_id, NULL );
    snd_seq_drain_output( data->seq );
#endif
    // Start our MIDI input thread.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);

    inputData_.doInput = true;
    int err = pthread_create(&data->thread, &attr, alsaMidiHandler, &inputData_);
    pthread_attr_destroy(&attr);
    if ( err ) {
      if ( data->subscription ) {
        snd_seq_unsubscribe_port( data->seq, data->subscription );
        snd_seq_port_subscribe_free( data->subscription );
        data->subscription = 0;
      }
      inputData_.doInput = false;
      errorString_ = "MidiInAlsa::openPort: error starting MIDI input thread!";
      error( RtMidiError::THREAD_ERROR, errorString_ );
      return;
    }
  }
}

void MidiInAlsa :: closePort( void )
{
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);

  if ( connected_ ) {
    if ( data->subscription ) {
      snd_seq_unsubscribe_port( data->seq, data->subscription );
      snd_seq_port_subscribe_free( data->subscription );
      data->subscription = 0;
    }
    // Stop the input queue
#ifndef AVOID_TIMESTAMPING
    snd_seq_stop_queue( data->seq, data->queue_id, NULL );
    snd_seq_drain_output( data->seq );
#endif
    connected_ = false;
  }

  // Stop thread to avoid triggering the callback, while the port is intended to be closed
  if ( inputData_.doInput ) {
    inputData_.doInput = false;
    int res = write( data->trigger_fds[1], &inputData_.doInput, sizeof(inputData_.doInput) );
    (void) res;
    if ( !pthread_equal(data->thread, data->dummy_thread_id) )
      pthread_join( data->thread, NULL );
  }
}

//*********************************************************************//
//  API: LINUX ALSA
//  Class Definitions: MidiOutAlsa
//*********************************************************************//

MidiOutAlsa :: MidiOutAlsa( const std::string clientName ) : MidiOutApi()
{
  initialize( clientName );
}

MidiOutAlsa :: ~MidiOutAlsa()
{
  // Close a connection if it exists.
  closePort();

  // Cleanup.
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  if ( data->vport >= 0 ) snd_seq_delete_port( data->seq, data->vport );
  if ( data->coder ) snd_midi_event_free( data->coder );
  if ( data->buffer ) free( data->buffer );
  snd_seq_close( data->seq );
  delete data;
}

void MidiOutAlsa :: initialize( const std::string& clientName )
{
  // Set up the ALSA sequencer client.
  snd_seq_t *seq;
  int result1 = snd_seq_open( &seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK );
  if ( result1 < 0 ) {
    errorString_ = "MidiOutAlsa::initialize: error creating ALSA sequencer client object.";
    error( RtMidiError::DRIVER_ERROR, errorString_ );
    return;
	}

  // Set client name.
  snd_seq_set_client_name( seq, clientName.c_str() );

  // Save our api-specific connection information.
  AlsaMidiData *data = (AlsaMidiData *) new AlsaMidiData;
  data->seq = seq;
  data->portNum = -1;
  data->vport = -1;
  data->bufferSize = 32;
  data->coder = 0;
  data->buffer = 0;
  int result = snd_midi_event_new( data->bufferSize, &data->coder );
  if ( result < 0 ) {
    delete data;
    errorString_ = "MidiOutAlsa::initialize: error initializing MIDI event parser!\n\n";
    error( RtMidiError::DRIVER_ERROR, errorString_ );
    return;
  }
  data->buffer = (unsigned char *) malloc( data->bufferSize );
  if ( data->buffer == NULL ) {
    delete data;
    errorString_ = "MidiOutAlsa::initialize: error allocating buffer memory!\n\n";
    error( RtMidiError::MEMORY_ERROR, errorString_ );
    return;
  }
  snd_midi_event_init( data->coder );
  apiData_ = (void *) data;
}

unsigned int MidiOutAlsa :: getPortCount()
{
	snd_seq_port_info_t *pinfo;
	snd_seq_port_info_alloca( &pinfo );

  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  return portInfo( data->seq, pinfo, SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE, -1 );
}

std::string MidiOutAlsa :: getPortName( unsigned int portNumber )
{
  snd_seq_client_info_t *cinfo;
  snd_seq_port_info_t *pinfo;
  snd_seq_client_info_alloca( &cinfo );
  snd_seq_port_info_alloca( &pinfo );

  std::string stringName;
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  if ( portInfo( data->seq, pinfo, SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE, (int) portNumber ) ) {
    int cnum = snd_seq_port_info_get_client(pinfo);
    snd_seq_get_any_client_info( data->seq, cnum, cinfo );
    std::ostringstream os;
    os << snd_seq_client_info_get_name(cinfo);
    os << " ";                                    // These lines added to make sure devices are listed
    os << snd_seq_port_info_get_client( pinfo );  // with full portnames added to ensure individual device names
    os << ":";
    os << snd_seq_port_info_get_port(pinfo);
    stringName = os.str();
    return stringName;
  }

  // If we get here, we didn't find a match.
  errorString_ = "MidiOutAlsa::getPortName: error looking for port name!";
  error( RtMidiError::WARNING, errorString_ );
  return stringName;
}

void MidiOutAlsa :: openPort( unsigned int portNumber, const std::string portName )
{
  if ( connected_ ) {
    errorString_ = "MidiOutAlsa::openPort: a valid connection already exists!";
    error( RtMidiError::WARNING, errorString_ );
    return;
  }

  unsigned int nSrc = this->getPortCount();
  if (nSrc < 1) {
    errorString_ = "MidiOutAlsa::openPort: no MIDI output sources found!";
    error( RtMidiError::NO_DEVICES_FOUND, errorString_ );
    return;
  }

	snd_seq_port_info_t *pinfo;
	snd_seq_port_info_alloca( &pinfo );
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  if ( portInfo( data->seq, pinfo, SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE, (int) portNumber ) == 0 ) {
    std::ostringstream ost;
    ost << "MidiOutAlsa::openPort: the 'portNumber' argument (" << portNumber << ") is invalid.";
    errorString_ = ost.str();
    error( RtMidiError::INVALID_PARAMETER, errorString_ );
    return;
  }

  snd_seq_addr_t sender, receiver;
  receiver.client = snd_seq_port_info_get_client( pinfo );
  receiver.port = snd_seq_port_info_get_port( pinfo );
  sender.client = snd_seq_client_id( data->seq );

  if ( data->vport < 0 ) {
    data->vport = snd_seq_create_simple_port( data->seq, portName.c_str(),
                                              SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ,
                                              SND_SEQ_PORT_TYPE_MIDI_GENERIC|SND_SEQ_PORT_TYPE_APPLICATION );
    if ( data->vport < 0 ) {
      errorString_ = "MidiOutAlsa::openPort: ALSA error creating output port.";
      error( RtMidiError::DRIVER_ERROR, errorString_ );
      return;
    }
  }

  sender.port = data->vport;

  // Make subscription
  if (snd_seq_port_subscribe_malloc( &data->subscription ) < 0) {
    snd_seq_port_subscribe_free( data->subscription );
    errorString_ = "MidiOutAlsa::openPort: error allocating port subscription.";
    error( RtMidiError::DRIVER_ERROR, errorString_ );
    return;
  }
  snd_seq_port_subscribe_set_sender(data->subscription, &sender);
  snd_seq_port_subscribe_set_dest(data->subscription, &receiver);
  snd_seq_port_subscribe_set_time_update(data->subscription, 1);
  snd_seq_port_subscribe_set_time_real(data->subscription, 1);
  if ( snd_seq_subscribe_port(data->seq, data->subscription) ) {
    snd_seq_port_subscribe_free( data->subscription );
    errorString_ = "MidiOutAlsa::openPort: ALSA error making port connection.";
    error( RtMidiError::DRIVER_ERROR, errorString_ );
    return;
  }

  connected_ = true;
}

void MidiOutAlsa :: closePort( void )
{
  if ( connected_ ) {
    AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
    snd_seq_unsubscribe_port( data->seq, data->subscription );
    snd_seq_port_subscribe_free( data->subscription );
    connected_ = false;
  }
}

void MidiOutAlsa :: openVirtualPort( std::string portName )
{
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  if ( data->vport < 0 ) {
    data->vport = snd_seq_create_simple_port( data->seq, portName.c_str(),
                                              SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ,
                                              SND_SEQ_PORT_TYPE_MIDI_GENERIC|SND_SEQ_PORT_TYPE_APPLICATION );

    if ( data->vport < 0 ) {
      errorString_ = "MidiOutAlsa::openVirtualPort: ALSA error creating virtual port.";
      error( RtMidiError::DRIVER_ERROR, errorString_ );
    }
  }
}

void MidiOutAlsa :: sendMessage( std::vector<unsigned char> *message )
{
  int result;
  AlsaMidiData *data = static_cast<AlsaMidiData *> (apiData_);
  unsigned int nBytes = message->size();
  if ( nBytes > data->bufferSize ) {
    data->bufferSize = nBytes;
    result = snd_midi_event_resize_buffer ( data->coder, nBytes);
    if ( result != 0 ) {
      errorString_ = "MidiOutAlsa::sendMessage: ALSA error resizing MIDI event buffer.";
      error( RtMidiError::DRIVER_ERROR, errorString_ );
      return;
    }
    free (data->buffer);
    data->buffer = (unsigned char *) malloc( data->bufferSize );
    if ( data->buffer == NULL ) {
    errorString_ = "MidiOutAlsa::initialize: error allocating buffer memory!\n\n";
    error( RtMidiError::MEMORY_ERROR, errorString_ );
    return;
    }
  }

  snd_seq_event_t ev;
  snd_seq_ev_clear(&ev);
  snd_seq_ev_set_source(&ev, data->vport);
  snd_seq_ev_set_subs(&ev);
  snd_seq_ev_set_direct(&ev);
  for ( unsigned int i=0; i<nBytes; ++i ) data->buffer[i] = message->at(i);
  result = snd_midi_event_encode( data->coder, data->buffer, (long)nBytes, &ev );
  if ( result < (int)nBytes ) {
    errorString_ = "MidiOutAlsa::sendMessage: event parsing error!";
    error( RtMidiError::WARNING, errorString_ );
    return;
  }

  // Send the event.
  result = snd_seq_event_output(data->seq, &ev);
  if ( result < 0 ) {
    errorString_ = "MidiOutAlsa::sendMessage: error sending MIDI message to port.";
    error( RtMidiError::WARNING, errorString_ );
    return;
  }
  snd_seq_drain_output(data->seq);
}

#endif // __LINUX_ALSA__

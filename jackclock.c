/*
 *                                                                              
 * Written by Nicolas Bouillot <nicolas@cim.mcgill.ca>                           
 *                                                                              
 * This program is free software; you can redistribute it and/or                
 * modify it under the terms of the GNU General Public License                  
 * as published by the Free Software Foundation; either version 2               
 * of the License, or (at your option) any later version.                       
 *                                                                              
 * See file LICENSE for further informations on licensing terms.                
 *                                                                              
 * This program is distributed in the hope that it will be useful,              
 * but WITHOUT ANY WARRANTY; without even the implied warranty of               
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                
 * GNU General Public License for more details.                                 
 *                                                                              
 * You should have received a copy of the GNU General Public License            
 * along with this program; if not, write to the Free Software                  
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.  
 *                                                                              
 * Based on PureData by Miller Puckette and others.                             
 *                                                                              
 */

#include "m_pd.h"
#include <pthread.h>
#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/types.h>
#include <unistd.h>

#ifdef NT
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )
#endif
 
//from s_stuff.h, pd-0.42-5
#define API_JACK 5
extern int sys_audioapi;

/* ------------------------ jackclock ----------------------------- */

static t_class *jackclock_class;

typedef struct _jackclock
{
  t_object x_obj;

  t_int is_connected_to_jack;
  t_int jack_sample_rate;

  jack_client_t *client;
  jack_position_t current; //from jack

  t_int looping;
  jack_nframes_t loop_begin;
  t_float loop_end;
  t_int loopeventlock;

  jack_transport_state_t transport_state;
  
  t_outlet *x_outlet;  //jack clock in ms when bang
  t_outlet *x_outlet0; //jack samplerate
  t_outlet *x_outlet1; //jack state
  t_outlet *x_outlet2; //bang when reaching the end of the loop (only in loop mode)

  pthread_attr_t attr;
  pthread_t jack_connect_thread;
} t_jackclock;


void jackclock_free(t_jackclock *x) 
{ 
  jack_release_timebase (x->client);
  jack_client_close (x->client); 
} 


void jackclock_locate(t_jackclock *x, t_floatarg pos_ms){
  jack_transport_locate (x->client, (jack_nframes_t) (pos_ms*x->jack_sample_rate));
}


void jackclock_freewheel(t_jackclock *x, t_floatarg onoff){

  if(sys_audioapi == API_JACK)
    jack_set_freewheel (x->client, (int)onoff);
  else
    post("jackclock: freewheel mode require to set jack as audio api");
}


void jackclock_locateframe(t_jackclock *x, t_floatarg pos_frame){
  jack_transport_locate (x->client, (jack_nframes_t) pos_frame);
}


void jackclock_start(t_jackclock *x){
  jack_transport_start(x->client);  	
}


void jackclock_stop(t_jackclock *x){
  jack_transport_stop(x->client);  	
}


void jackclock_setloop(t_jackclock *x, jack_nframes_t begin_frame, jack_nframes_t end_frame){
  if(begin_frame >= end_frame){
    post("jackclock: reverse playing not supported\n");}
  else{
    x->loop_begin=begin_frame; 
    x->loop_end=end_frame;
    x->looping=1;
    jack_transport_locate (x->client, (jack_nframes_t) begin_frame);
  }
}


void jackclock_loop(t_jackclock *x, t_floatarg begin_ms, t_floatarg end_ms ){
  jackclock_setloop(x,  (jack_nframes_t)(begin_ms*x->jack_sample_rate), 
                    (jack_nframes_t)(end_ms*x->jack_sample_rate));
}


void jackclock_loopframe(t_jackclock *x, t_floatarg begin_frame, t_floatarg end_frame ){
  jackclock_setloop(x,  (jack_nframes_t)begin_frame, 
                    (jack_nframes_t)end_frame);
}


void jackclock_unloop(t_jackclock *x){
  x->looping=0;
}


void jackclock_bang(t_jackclock *x) 
{
  if(x->is_connected_to_jack) 
  {

    jack_transport_state_t old_transport_state=x->transport_state;
    x->transport_state = jack_transport_query (x->client, &x->current);
    
    float samplerate=(float)jack_get_sample_rate (x->client);
    float currenttime=(float)x->current.frame;

    outlet_float(x->x_outlet, currenttime);
    if(samplerate != (float)x->jack_sample_rate)
    {
      outlet_float(x->x_outlet0, samplerate);
      x->jack_sample_rate=samplerate;
    }

    if(old_transport_state != x->transport_state)
    {
      switch (x->transport_state) {
        case JackTransportStopped:
          outlet_float(x->x_outlet1,0);
          break;
        case JackTransportRolling:
          outlet_float(x->x_outlet1,1);
          break;
        case JackTransportStarting:
          outlet_float(x->x_outlet1,2);
          break;
        default:
          outlet_float(x->x_outlet1,3);
      }
    }
  }
}


void jack_shutdown (void *arg) 
{ 
  t_jackclock *x=(t_jackclock *)arg; 
  x->is_connected_to_jack=0; 
} 


void jack_timebase_callback (jack_transport_state_t state, jack_nframes_t nframes, jack_position_t* pos, int new_position, void *arg) 
{
  t_jackclock *x=(t_jackclock *)arg;

  if(x->looping)
  {
    jack_transport_state_t current_transport_state = jack_transport_query (x->client, &x->current);
  
    if(x->current.frame >= x->loop_end){
      if(!x->loopeventlock)
      {
        jack_transport_locate (x->client, (jack_nframes_t) x->loop_begin);
        outlet_bang(x->x_outlet2);
        x->loopeventlock=1;
      }
      else x->loopeventlock=0;
    }
  }
}
 
void *jackconnect (void *v_param) 
{ 
  t_jackclock *x=(t_jackclock *)v_param; 

  while(!x->is_connected_to_jack) 
  { 
    if ((x->client = jack_client_open ("jackclock", JackNullOption, NULL)) == 0) { 
      post ("jackclock: jack server not running? retry...\n"); 
    } 
    else 
    { 
      jack_on_shutdown (x->client, jack_shutdown, (void *)x); 
      jack_set_timebase_callback(x->client, 0,  jack_timebase_callback, (void *)x); 

      if (jack_activate (x->client)) post ("jackclock: cannot activate client"); 
      else x->is_connected_to_jack=1; 
    } 
    sleep(1); 
  } 
  return NULL;
} 




static void *jackclock_new(void)
{
  t_jackclock *x = (t_jackclock *)pd_new(jackclock_class);

  x->is_connected_to_jack=0; 
  x->jack_sample_rate=0; 
  x->looping=0;
  x->loop_begin=0;
  x->loop_end=0;
  x->loopeventlock=0;

  x->x_outlet = outlet_new(&x->x_obj, &s_float); 
  x->x_outlet0 = outlet_new(&x->x_obj, &s_float); 
  x->x_outlet1 = outlet_new(&x->x_obj, &s_float); 
  x->x_outlet2 = outlet_new(&x->x_obj, &s_float); 
    
  pthread_attr_init(&(x->attr)); 
  pthread_attr_setdetachstate(&(x->attr), PTHREAD_CREATE_JOINABLE); 
    
  pthread_create(&x->jack_connect_thread, 
		 &(x->attr), 
		 jackconnect, 
		 (void*) x); 
    
  pthread_attr_destroy(&(x->attr)); 
  return (x);
}


void jackclock_setup(void)
{
  jackclock_class = class_new(gensym("jackclock"), 
                              (t_newmethod)jackclock_new, 
                              (t_method) jackclock_free,
                              sizeof(t_jackclock), 
                              0, 
                              A_DEFFLOAT,
                              0);
  class_addmethod(jackclock_class, (t_method)jackclock_locate, 
		  gensym("locate"), A_FLOAT, 0);
  class_addmethod(jackclock_class, (t_method)jackclock_locateframe, 
		  gensym("locateframe"), A_FLOAT, 0);
  class_addmethod(jackclock_class, (t_method)jackclock_start, 
		  gensym("start"), 0);
  class_addmethod(jackclock_class, (t_method)jackclock_stop, 
		  gensym("stop"), 0);
  class_addmethod(jackclock_class, (t_method)jackclock_loop, 
		  gensym("loop"), A_FLOAT, A_FLOAT,0);
  class_addmethod(jackclock_class, (t_method)jackclock_loopframe, 
		  gensym("loopframe"), A_FLOAT, A_FLOAT,0);
  class_addmethod(jackclock_class, (t_method)jackclock_unloop, 
		  gensym("unloop"),0);
  class_addmethod(jackclock_class, (t_method)jackclock_freewheel, 
		  gensym("freewheel"),  A_FLOAT,0);
  class_addbang(jackclock_class, jackclock_bang);

  class_sethelpsymbol(jackclock_class, gensym("help-jackclock"));
}

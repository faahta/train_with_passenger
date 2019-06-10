#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<pthread.h>
#include<time.h>
#include<semaphore.h>
#include<stdbool.h>
#include<errno.h>

#define L 200
#define T_NEW_PASS 3
#define TRAIN_CAPACITY 100
#define MAX_PASSENGERS 512
#define T_MAX 15 //max time to load passengers
#define T_TR 15
#define N_STATIONS 4
#define N_TRAINS 2

int fd1[2], fd2[2]; //file descriptors for pipe (for communication between train and station threads)


typedef struct Station{
	int sid;
	int next_st;
	int train;
	sem_t *empty, *full, *sem, *red_sem;
	int passengers[MAX_PASSENGERS];	/*shared buffer*/
	int in, out;
}Station;

typedef struct Passenger{
	int pid;
	int st_station;
	int dst_station;
}Passenger;

typedef struct Train{
	int tid;
	int station;
	int passengers_in[TRAIN_CAPACITY]; /*buffer managed by train*/
	int passengers_out;
	int npassengers;
	FILE *fp;
	pthread_mutex_t w_lock; //to write to file
}Train;

Train *train;
Station *station;
Passenger *passenger;
pthread_mutex_t lock;	//ME to read and write to shared buffer
sem_t *S; //for signaling train threads to start

static void 
put(int st, int id){
	sem_wait(station[st].empty);
	station[st].passengers[station[st].in] = id;
	printf("passenger %d is in station %d\n",id, st);
	station[st].in = (station[st].in + 1) % MAX_PASSENGERS;
	sem_post(station[st].full);
}

static int
get (int st){
  	int data;
	struct timespec ts;
	ts.tv_sec = time(NULL) + T_MAX;
	ts.tv_nsec = 0;
	int s;
	while((s = sem_timedwait(station[st].full, &ts)) == -1 && errno == EINTR)
		continue;
	if (s == -1) {
		if (errno == ETIMEDOUT){
			printf("train %d sem_timedwait() timed out\n", station[st].train);
			return -1;	
		}
		else{
			perror("sem_timedwait");
			return -1;
		}
	} else
		printf("train %d sem_timedwait() succeeded\n", station[st].train);
	data = station[st].passengers[station[st].out];
	station[st].out = (station[st].out + 1) % MAX_PASSENGERS;
	//sem_post(station[st].empty);
	return data;
}

/**************************THREADS START ROUTINE*************************************/
/************************************************************************************/
static void * 
station_thread(void * argst){
	pthread_detach(pthread_self());
	int *id = (int *)argst;
	int sid = *id;
	printf("Station %d: waiting for train info through pipe...\n", sid);
	Train tr;
	while(1){
		int result;
		
		if(station[sid].train == 0)
			result = read(fd1[0], &tr, sizeof(tr));
		if(station[sid].train == 1)
			result = read(fd2[0], &tr, sizeof(tr));
		if(result != 1){
			perror("pipe read: ");
		}
		printf("/********************************STATION THREAD***************************************/\n");
		printf("Train %d: Station %d: passengers_in %d: passengers_out %d \n",tr.tid, tr.station, tr.npassengers, tr.passengers_out);	
		printf("/************************************************************************************/\n");
	}		
}
static void * 
passenger_thread(void * argps){
	pthread_detach(pthread_self());
	int *id = (int *)argps;
	int pid = *id;
	
	pthread_mutex_lock(&lock);
		/*randomly select start and destination stations*/
		srand(time(NULL));
		int st = ((rand() % N_STATIONS));
		//sleep(1);
		int dst = ((rand() % N_STATIONS));
		
		passenger[pid].pid = pid;
		passenger[pid].st_station = st;
		passenger[pid].dst_station = dst;
	
		printf("station %d in: %d\n", st, station[st].in);
		put(st, pid);
		if(station[st].in == MAX_PASSENGERS)
			put(st, -1);
	pthread_mutex_unlock(&lock);
	
	sem_wait(station[passenger[pid].dst_station].sem);
	printf("passenger %d reached destination...\n", pid);
	pthread_exit(NULL);
}

static void * 
train_thread(void * argtr){
	pthread_detach(pthread_self());
	sem_wait(S);
	int *id = (int *)argtr;
	int tid = *id;	
	int i;
	while(1){
		sleep(1);
		/*wait for red semaphore*/
		int st = train[tid].station; /*current station*/
		if(train[0].station == train[1].station){		
			fprintf(stdout, "train %d waiting for station %d to be released\n", tid, st);
			/*the fastest train will pass this red semaphore*/
			sem_wait(station[st].red_sem); 
		}
		station[st].train = tid;	/*current train at this station*/
		
		pthread_mutex_lock(&lock);
			while(train[tid].npassengers <= TRAIN_CAPACITY){	
				int pid = get(st);
				if(pid == -1){
					break;
				}
				train[tid].passengers_in[train[tid].npassengers] = pid;
				train[tid].npassengers++;		
			}	
		pthread_mutex_unlock(&lock);
		
		/*travel to next station*/
		sleep(T_TR);
		int next_station = station[st].next_st;
		train[tid].station = next_station;
		
		printf("train %d arrived to station %d...\n",tid, st);
		
		sem_post(station[st].red_sem);
		
		pthread_mutex_lock(&lock);
			/*check passengers who reached their destination*/			
			for(i=0; i<train[tid].npassengers; i++){
				int p = train[tid].passengers_in[i];
				//check if each passenger reaches its destination
				if(passenger[p].dst_station == train[tid].station){
					sem_post(station[next_station].sem); /**/
					train[tid].npassengers--;
					train[tid].passengers_out++;
				}
			}
		pthread_mutex_unlock(&lock);	
		/***write to log file ***/
		pthread_mutex_lock(&lock);
			if((train[tid].fp = fopen("log.txt", "a")) != NULL){
				char first[train[tid].npassengers];
				char second[train[tid].npassengers]; 
				char first_l[MAX_PASSENGERS];
				char second_l[MAX_PASSENGERS];
				/*first line => the train's current situation*/				
				for(i=0; i<train[tid].npassengers; i++){
					if(i == 0){
						sprintf(first_l, "%d ", train[tid].passengers_in[0]);	
					} else{
						sprintf(first, "%d ", train[tid].passengers_in[i]);	
						strcat(first_l, first);
					}	
				}	
				fprintf(train[tid].fp, "Train %d: station:%d npassengers:%d passengers_in: %s\n",tid,train[tid].station,
																	train[tid].npassengers, first_l);	
				/*second line => destination station of each passenger*/
				for(i=0; i<train[tid].npassengers; i++){
					if(i == 0){
						sprintf(second, "%d ", passenger[train[tid].passengers_in[0]].dst_station);	
					} else{
						sprintf(second, "%d ",  passenger[train[tid].passengers_in[i]].dst_station);
						strcat(second_l, second);
					}
							
				}
				fprintf(train[tid].fp, "Destinations: %s\n\n", second_l);
				fprintf(train[tid].fp, "%s\n", "-------------------------------------------------------------------------------------");
				fclose(train[tid].fp);
			} else{ perror("file write error:");}
		pthread_mutex_unlock(&lock);
			
		/*pipe write to station thread*/
		int res1, res2;
		if(tid == 0){
			res1 = write(fd1[1], &train[tid], sizeof(train[tid]));
			if(res1 != 1){
				perror("pipe write: ");
			}
		}		
		if(tid == 1){
			res2 = write(fd2[1], &train[tid], sizeof(train[tid]));
			if(res2 != 1){
				perror("pipe write: ");
			}
		}
		train[tid].passengers_out = 0;	
		/*release the station for the next train*/
		//sem_post(station[train[tid].station].red_sem);	
	}
	
	pthread_exit(NULL);
}



void printstations(int src, int dst){
	printf("ST%d<------->ST%d\n", src, dst);
}

void
init(){
	station = (Station *)malloc(N_STATIONS * sizeof(Station));
	train = (Train *)malloc(N_TRAINS * sizeof(Train));
	passenger = (Passenger *)malloc(L * sizeof(Passenger));
	S  = (sem_t *)malloc(sizeof(sem_t));
	pthread_mutex_init(&lock, NULL);
	sem_init(S, 0, 0);
	int res = pipe(fd1);
	if(res < 0){
		perror("pipe");
		exit(1);
	}
	res = pipe(fd2);
	if(res < 0){
		perror("pipe");
		exit(1);
	}
}

void
setup(){
	int i;
	srand(time(NULL));
	//setup stations
	for(i=0; i<N_STATIONS; i++){
		station[i].sid = i;
		
		if(i == N_STATIONS-1){
			station[i].next_st = 0;
		} else{
			station[i].next_st = i+1;
		}
		station[i].in = 0;
		station[i].out = 0;
		
		station[i].empty = (sem_t *)malloc(sizeof(sem_t));
		sem_init(station[i].empty, 0, MAX_PASSENGERS);
		station[i].full = (sem_t *)malloc(sizeof(sem_t));
		sem_init(station[i].full, 0, 0);
		station[i].sem = (sem_t *)malloc(sizeof(sem_t));
		sem_init(station[i].sem, 0, 0);
		
		station[i].red_sem = (sem_t *)malloc(sizeof(sem_t));
		sem_init(station[i].red_sem, 0, 1);	
		printstations(station[i].sid, station[i].next_st);
	}
	//setup train
	int r;
	for(i=0; i<N_TRAINS; i++){
		train[i].tid = i;
		r = ((rand() % N_STATIONS));
		train[i].station = r;
		train[i].npassengers = 0;
		train[i].passengers_out = 0;
		pthread_mutex_init(&train[i].w_lock, NULL);		
	}

}

int 
main(int argc, char *argv[]){

	pthread_t *th_tr, *th_st,*th_psn;
	int i;
	
	init();
	setup();
	
	th_tr = (pthread_t *)malloc(N_TRAINS * sizeof(pthread_t));
	th_st = (pthread_t *)malloc(N_STATIONS * sizeof(pthread_t));
	th_psn = (pthread_t *)malloc(L * sizeof(pthread_t));
	
	//create train thread
	int *ti;
	for(i=0; i<N_TRAINS; i++){
		ti = (int *)malloc(sizeof(int));
		*ti = i;
		pthread_create(&th_tr[i], NULL, train_thread, (void *)ti);
	}
	//create station thread
	int *si;
	for(i=0; i<N_STATIONS; i++){
		si = (int *)malloc(sizeof(int));
		*si = i;
		pthread_create(&th_st[i], NULL, station_thread, (void *)si);
	}
	
	int j;
	j=0;
	//create passenger threads
	int *pi;
	for(j=0; j < L; j++){
		sleep((rand() % T_NEW_PASS));
		pi = (int *)malloc(sizeof(int));
		*pi = j;
		pthread_create(&th_psn[i], NULL, passenger_thread, (void *)pi);
	}
	
	for(i=0; i<2; i++){
		sem_post(S);
		//sem_post(train[i].red_sem[train[i].station]);
	}
	
	pthread_exit((void *)pthread_self());
	
}

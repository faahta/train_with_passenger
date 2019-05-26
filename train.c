#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<pthread.h>
#include<time.h>
#include<semaphore.h>
#include<stdbool.h>
#include<errno.h>

#define L 100
#define T_NEW_PASS 3
#define TRAIN_CAPACITY 30
#define ST_QUEUE_CAPACITY 100
#define T_MAX 3 //max time to load passengers
#define T_TR 10
#define N_STATIONS 4
#define N_TRAINS 2

int fd[2]; //file descriptor for creating pipe (to communicate between train and station threads)

typedef struct StationQueue{
	int *passengers;
	int capacity;
	int front, rear, size;
}StationQueue;

typedef struct Station{
	int sid;
	int next_st;
	sem_t *sem;
	sem_t *pipe_sem;
	StationQueue *st_queue;	
	pthread_mutex_t r_lock; //to read from file
}Station;

typedef struct Passenger{
	int pid;
	int st_station;
	int dst_station;
}Passenger;

typedef struct Train{
	int tid;
	int station;
	int dst_station;
	int *passengers_in;
	int *passengers_out;
	int npassengers;
	sem_t *red_sem[N_STATIONS];
	pthread_mutex_t w_lock; //to write to file
}Train;

Train *train;
Station *station;
Passenger *passenger;

sem_t *S; //for signaling train threads to start
FILE *fp;


int isFull(StationQueue *st_queue){
	return (st_queue->size == st_queue->capacity);
}
void enqueuePassenger(StationQueue *st_queue, int passenger_id){
	if(isFull(st_queue)){
		printf("station capacity exceeded\n");
		return;
	}
	st_queue->rear = (st_queue->rear + 1) % st_queue->capacity;
	st_queue->passengers[st_queue->rear] = passenger_id;
	st_queue->size += 1;
}
int isEmpty(StationQueue *st_queue){
	return (st_queue->size==0);
}
int dequeuePassenger(StationQueue *st_queue){
	if(isEmpty(st_queue)){
		printf("No passengers in this station\n");
		return -1;
	}
	int pid = st_queue->passengers[st_queue->front];
	st_queue->front = (st_queue->front + 1) %st_queue->capacity;
	st_queue->size -= 1;
	
	return pid;
	
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
	sem_wait(station[sid].pipe_sem);
	/*pipe read*/
	pthread_mutex_lock(&station[sid].r_lock);
		int result = read(fd[0], &tr, sizeof(tr));
		if(result != 1){
			perror("pipe read: ");
			//exit(3);
		}
		printf("/********************************STATION THREAD***************************************/\n");
		printf("Train %d: Station %d: passengers_in %d: passengers_out %ld \n",tr.tid, tr.station, tr.npassengers, 
															sizeof(tr.passengers_out)/sizeof(tr.passengers_out[0]));	
		printf("/************************************************************************************/\n");
		
	pthread_mutex_lock(&station[sid].r_lock);
	pthread_exit(NULL);	
}
static void * 
passenger_thread(void * argps){
	pthread_detach(pthread_self());
	int *id = (int *)argps;
	int pid = *id;
	
	//randomly select start and destination stations
	srand(time(NULL));
	int st = ((rand() % N_STATIONS));
	sleep(1);
	int dst = ((rand() % N_STATIONS));
	
	passenger[pid].pid = pid;
	
	passenger[pid].st_station = st;
	passenger[pid].dst_station = dst;
	//printf("passenger %d: start station: %d dest. station: %d\n",pid, passenger[pid].st_station, passenger[pid].dst_station);
	//enque the passenger to the selected start station
	enqueuePassenger(station[passenger[pid].st_station].st_queue, pid);
	//printf("passenger %d enqueued to station %d\n", pid, passenger[pid].st_station);
	
	sem_post(station[passenger[pid].st_station].sem);
	sem_wait(station[passenger[pid].dst_station].sem);
	//printf("passenger %d arrived to destination %d\n", pid, passenger[pid].dst_station);
	pthread_exit(NULL);
}

static void * 
train_thread(void * argtr){
	pthread_detach(pthread_self());
	sem_wait(S);
	int *id = (int *)argtr;
	int tid = *id;	
	while(1){
		/*wait for red semaphore*/
		int st = train[tid].station; //current station
		if(tid == 0){
			if(train[0].station == train[1].station){		
				sem_wait(train[tid].red_sem[st]); 
				fprintf(stdout, "train %d station %d released\n", tid, st);}
		}
		if(tid == 1){
			if(train[1].station == train[0].station){
				sem_wait(train[tid].red_sem[st]);
				fprintf(stdout, "train %d station %d released\n", tid, st);}
		}
								
		/*wait for passengers to get in*/
		int s;
		struct timespec ts;
		ts.tv_sec = time(NULL) + T_TR;
		ts.tv_nsec = 0;
		s = sem_timedwait(station[st].sem, &ts);;
		if(s == -1){
			if(errno == ETIMEDOUT){printf("timeout\n");}
			else{ perror("sem_timedwait"); break; }
		} else{	
			continue;		
		}
		/*no timeout: //load passengers => dequeue passengers*/		
		printf("train %d trying load passengers at station %d...\n",tid, st);
		int i, qsize;
		qsize = station[st].st_queue->size;
		for(i=0; i<qsize; i++){
			if(train[tid].npassengers <= TRAIN_CAPACITY){
				int pid = dequeuePassenger(station[st].st_queue);
				train[tid].passengers_in[i] = pid;
				train[tid].npassengers++; }		
		}
		printf("train %d loaded %d passengers at station %d...\n",tid, train[tid].npassengers, st);
		/*travel to next station*/
		sleep(3);
		int next_station = station[st].next_st;
		train[tid].station = next_station;
		printf("train %d arrived to station %d...\n",tid, st);
		/*check passengers who reached their destination*/			
		for(i=0; i<train[tid].npassengers; i++){
			int p = train[tid].passengers_in[i];
			//check if each passenger reaches its destination
			if(passenger[p].dst_station == train[tid].station){
				sem_post(station[next_station].sem); /**/
				train[tid].npassengers--;
				train[tid].passengers_out[i] = passenger[p].pid;
			}
		}
printf("train %d unloaded %ld passengers at station %d...\n",tid, sizeof(train[tid].passengers_out)/sizeof(train[tid].passengers_out[0]) , st);
		
		/***write to log file ***/
		pthread_mutex_lock(&train[tid].w_lock);
			if((fp = fopen("log.txt", "a")) != NULL){
				char first[train[tid].npassengers];
				//first = (char *)malloc(train[tid].npassengers * sizeof(char));
				char second[train[tid].npassengers]; 
				//second = (char *)malloc(train[tid].npassengers * sizeof(char));
				char first_l[100];
				char second_l[100];
				/*first line => the train's current situation*/				
				for(i=0; i<train[tid].npassengers; i++){
					sprintf(first + i, "%d ", train[tid].passengers_in[i]);			
				}	
				fprintf(fp, "Train %d: station:%d npassengers:%d passengers_in: %s\n",train[tid].tid,train[tid].station,
																	train[tid].npassengers, first);	
				/*second line => destination station of each passenger*/
				
				for(i=0; i<train[tid].npassengers; i++){
					sprintf(second + i, "%d ", passenger[train[tid].passengers_in[i]].dst_station);			
				}
				fprintf(fp, "Destinations: %s\n\n", second);
				fclose(fp);
			} else{ perror("file write error:");}
					
			/*pipe write to station thread*/
			int result;
			result = write(fd[1], &train[tid], sizeof(train[tid]));
			if(result != 1){
				perror("pipe write: ");
				//exit(2);
			}
			sem_post(station[train[tid].station].pipe_sem);
			printf("train %d finished writing to log and pipe...\n",tid);			
		pthread_mutex_unlock(&train[tid].w_lock);
		/*release the station for the next train*/
		sem_post(train[tid].red_sem[train[tid].station]);
		
		
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
	sem_init(S, 0, 0);
	int res = pipe(fd);
	if(res < 0){
		perror("pipe");
		exit(1);
	}
}

void
setup(){
	int i;
	srand(time(NULL));
	//setup stations and their queues
	for(i=0; i<N_STATIONS; i++){
		station[i].sid = i;
		if(i == N_STATIONS-1){
			station[i].next_st = 0;
		} else{
			station[i].next_st = i+1;
		}
		station[i].sem = (sem_t *)malloc(sizeof(sem_t));
		sem_init(station[i].sem, 0, 0);
		station[i].pipe_sem = (sem_t *)malloc(sizeof(sem_t));
		sem_init(station[i].pipe_sem, 0, 0);
		//initialize station queue
		station[i].st_queue = (StationQueue *)malloc(sizeof(StationQueue));
		station[i].st_queue->capacity = ST_QUEUE_CAPACITY;
		station[i].st_queue->front = station[i].st_queue->size = 0;
		station[i].st_queue->rear = ST_QUEUE_CAPACITY - 1;
		station[i].st_queue->passengers = (int *)malloc(station[i].st_queue->capacity * sizeof(int));
		pthread_mutex_init(&station[i].r_lock, NULL);
		printstations(station[i].sid, station[i].next_st);
	}
	//setup train
	int r;
	for(i=0; i<N_TRAINS; i++){
		train[i].tid = i;
		r = ((rand() % N_STATIONS));
		train[i].station = r;
		train[i].dst_station = station[r].next_st;
		train[i].passengers_in = (int *)malloc(TRAIN_CAPACITY * sizeof(int));
		train[i].passengers_out = (int *)malloc(TRAIN_CAPACITY * sizeof(int));
		pthread_mutex_init(&train[i].w_lock, NULL);
		int j;
		for(j=0; j<N_STATIONS; j++){
			train[i].red_sem[j] = (sem_t *)malloc(sizeof(sem_t));
			sem_init(train[i].red_sem[j], 0, 0);			
		}
		
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
		sem_post(train[i].red_sem[train[i].station]);
	}
	
	pthread_exit((void *)pthread_self());
	
	
}

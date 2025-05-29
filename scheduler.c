#include <stdio.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <stdbool.h>
#include <time.h>


#define MAX_CARGO_COUNT 200
#define MAX_AUTH_STRING_LEN 100
#define MAX_NEW_REQUESTS 100
#define MAX_SOLVERS 8
#define MAX_CATEGORY 25
#define MAX_DOCKS 30
#define MAX_SHIP_REQUESTS 1100  


typedef struct ShipRequest {
    int shipId;
    int timestep;
    int category;
    int direction;      
    int emergency;     
    int waitingTime;    
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
} ShipRequest;

typedef struct MainSharedMemory {
    char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
    ShipRequest newShipRequests[MAX_NEW_REQUESTS];
} MainSharedMemory;

typedef struct MessageStruct {
    long mtype;
    int timestep;
    int shipId;
    int direction;
    int dockId;
    int cargoId;
    int isFinished;
    union {
        int numShipRequests;
        int craneId;
    };
} MessageStruct;

typedef struct SolverRequest {
    long mtype;
    int dockId;
    char authStringGuess[MAX_AUTH_STRING_LEN];
} SolverRequest;

typedef struct SolverResponse {
    long mtype;
    int guessIsCorrect;
} SolverResponse;

 typedef struct Dock {
    int id;
    int category;
    int craneCapacities[MAX_CATEGORY];
    int numCranes;
    bool isOccupied;
    int occupiedByShipId;
    int occupiedByDirection;
    int dockingTimestep;
    int lastCargoMovedTimestep;
    bool cargoFullyMoved;
    bool craneUsed[MAX_CATEGORY];
} Dock;

//structure to hold ship info
typedef struct Ship {
    int id;
    int direction;     
    int category;
    int emergency;     
    int waitingTime;
    int arrivalTimestep;
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
    int cargoProcessed;
    int dockId;
    int status;        
    int leaveTimestep; 
} Ship;

typedef struct Thr_data {
    int solverMsgid;        
    int startIndex;         
    int endIndex;           
    char **guesses;         
    char *correctGuess;     
    int length;             
    bool *found;            
    int dockId;             
    pthread_mutex_t *mutex; 
} Thr_data;

typedef struct {
    int dockId;
    int status;
} datastatus;


MainSharedMemory *sharedMemory;
Dock docks[MAX_DOCKS];
Ship ships[MAX_SHIP_REQUESTS];
int n;
int nships = 0;
int curr_timestep = 1;
int solver_ids[MAX_SOLVERS];
int m;
int shmid, mqid;
int sid;


//debugging error
void check_dock_processes(int dockId) {
    printf(" dock loading for dock #%d...\n", dockId);
    
    printf("Unloading dock of dockId: %d",dockId);
    usleep(1000);
}

 //Debugged till here 
// Process new ship requests from validation
void logIPCUsage(int msgCount, int shmReads, int shmWrites) {
    printf("[IPC Monitor] Messages sent: %d | Shared memory reads: %d | writes: %d\n",
           msgCount, shmReads, shmWrites);
    if (msgCount > 1000 || shmReads + shmWrites > 5000) {
        printf("[Warning] High IPC activity detected. Consider optimizing usage.\n");
    }
}


// Find a ship by its ID and direction
Ship* find_ship(int shipId, int dirn) {
    for (int i = 0; i < nships; i++) {
        if (ships[i].id == shipId && ships[i].direction == dirn) {
            return &ships[i];
        }
    }
    return NULL;
}
void new_ship_req(int nreq) {
    int i = 0;
    while(i < nreq){
        ShipRequest req = sharedMemory->newShipRequests[i];
       
        // Check if this is a returning ship
        Ship *existingShip = find_ship(req.shipId, req.direction);
        if (existingShip != NULL && existingShip->status == 0) {
            // Update the existing ship's arrival timestep
            existingShip->arrivalTimestep = req.timestep;
            i++;
            continue;
        }
       
        //add new ship
        Ship newShip;
        newShip.id = req.shipId;
        newShip.numCargo = req.numCargo;
        newShip.dockId = -1;
        newShip.direction = req.direction;
        newShip.category = req.category;
        newShip.waitingTime = req.waitingTime;
        newShip.emergency = req.emergency;
        newShip.arrivalTimestep = req.timestep;
        newShip.cargoProcessed = 0;
        newShip.status = 0;  //waiting
       
        //cargo weights copied
        int j = 0;
        while (j < req.numCargo) {
            newShip.cargo[j] = req.cargo[j];
            j++;
        }
       
        ships[nships++] = newShip;
        i++;
    }
}
//debugging error
void ValidationNotify(datastatus msg) {
    if (msg.status > 0) {
        // if(msgsnd(1000,2,sizeof(datastatus)-sizeof(long),0)==-1){
        //     perror("Message sending failed!");
        // }
        printf("Message sent successful");
    }
}


bool check_time(Ship *ship, int curr_timestep) {
    if(ship->direction== -1 ||ship->emergency==1){
        return true;
    }

    return curr_timestep<= (ship->arrivalTimestep + ship->waitingTime);
}
//debugging error
int cargo_mass_op(int d_id, ShipRequest *ship) {
    int gms = 0;
    for(int i = 1; i <= d_id ; i++){
        gms += ship->numCargo;
    }
    return gms;
}

//comparison function for sorting ships by priority
int cmp_ships(const void *a, const void *b) {
    const Ship *sa = (const Ship *)a;
    const Ship *sb = (const Ship *)b;
   
    if (sa->status==2 && sb->status!=2) 
        return 1;
    if (sa->status==2 && sb->status==2) 
        return 0;
    if (sb->status==2 && sa->status!=2) 
        return -1;
   
    if (sa->status!=sb->status) {
        return sa->status-sb->status;
    }
   
    if (sa->emergency!=sb->emergency) {
        return sb->emergency-sa->emergency;
    }
   
    if (sa->emergency==0 && sb->emergency==0 && sa->direction==1 && sb->direction==1) {
        int remainingA = (sa->arrivalTimestep + sa->waitingTime) - curr_timestep;
        int remainingB = (sb->arrivalTimestep + sb->waitingTime) - curr_timestep;
        if (remainingA!=remainingB) {
            int k =remainingA-remainingB;
            return k;//debugged till here
        }
    }
    if (sa->direction!=sb->direction) {
        return sb->direction-sa->direction;
    }
    return sa->arrivalTimestep-sb->arrivalTimestep;
}

//debugging error

void dock_locking(){
    bool dockLocks[MAX_DOCKS] = {false};

    for (int i = 0; i < MAX_DOCKS; i++){
        if (i%2==0){
            dockLocks[i]=true; //simulate locked dock
            printf("[DockLock] Dock %d is currently locked by a thread.\n", i);
        } else{
            printf("[DockLock] Dock %d is free.\n", i);
        }
    }
}


int calc_optDock(Ship *ship) {
    if (ship->emergency == 1) {
        int bestDock= -1;
        int minCategory= MAX_CATEGORY + 1;
        int i=0;

        if (n>0){
            do{
                if(!docks[i].isOccupied && docks[i].category>=ship->category) {
                    if (docks[i].category < minCategory) {
                        minCategory= docks[i].category;
                        bestDock= i;
                    }
                }
                i++;
            }while(i<n);
        }

        return bestDock;
    }

    int i=0;
    if (n>0){
        do{
            if(!docks[i].isOccupied && docks[i].category == ship->category){
                return i;
            }
            i++;
        }while(i<n);
    }

    int bestDock= -1;
    int minCategory= MAX_CATEGORY+1;
    i=0;

    if (n>0){
        do{
            if(!docks[i].isOccupied && docks[i].category >= ship->category){
                if(docks[i].category < minCategory){
                    minCategory= docks[i].category;
                    bestDock= i;
                }
            }
            i++;
        }while(i < n);
    }

    return bestDock;
}

 

void msg_to_val(int mtype, int shipId, int direction, int dockId, int cargoId, int craneId){
    MessageStruct m;
    m.mtype= mtype;
    m.shipId= shipId;
    m.direction= direction;
    m.dockId= dockId;
    m.craneId= craneId;
    m.cargoId= cargoId;
    if (msgsnd(mqid,&m,sizeof(MessageStruct)-sizeof(long),0)== -1) {
        perror("Error sending message to validation");
        exit(EXIT_FAILURE);
    }
}
//debugged till here

void load_cargo(Ship *ship, Dock *dock) {
    int count1 = 0;
while (count1 < dock->numCranes) {
    dock->craneUsed[count1] = false;
    count1++;
}  
int cargoIdx = ship->cargoProcessed;
while (cargoIdx < ship->numCargo) {
    int cargoWeight = ship->cargo[cargoIdx];
    int bestCraneIdx = -1; // find crane with capacity closest to cargo weight
    int minWaste = INT_MAX;

    int craneidx = 0;
    while (craneidx < dock->numCranes) {
        if (!dock->craneUsed[craneidx] && dock->craneCapacities[craneidx] >= cargoWeight) {
            int waste = dock->craneCapacities[craneidx] - cargoWeight;
            if (waste < minWaste) {
                minWaste = waste;
                bestCraneIdx = craneidx;
            }
        }
        craneidx++;
    }

    if (bestCraneIdx != -1) {
        dock->craneUsed[bestCraneIdx] = true;
        msg_to_val(4, ship->id, ship->direction, dock->id, cargoIdx, bestCraneIdx);
        ship->cargoProcessed++;
        cargoIdx++;
    } else {
        break;
    }
}
}


void unload_cargo(Ship *ship, Dock *dock){
    for (int i=0; i<dock->numCranes; i++) {
        dock->craneUsed[i]=false;
    }
    
    
    int cargoIdx = ship->cargoProcessed;
    while (cargoIdx < ship->numCargo) {
        int cargoWeight = ship->cargo[cargoIdx];
        int bestCraneIdx = -1;
        int minWaste = INT_MAX;
    
        int craneIdx = 0;
        while (craneIdx < dock->numCranes) {
            if (!dock->craneUsed[craneIdx] && dock->craneCapacities[craneIdx] >= cargoWeight) {
                int waste = dock->craneCapacities[craneIdx] - cargoWeight;
                if (waste < minWaste) {
                    minWaste = waste;
                    bestCraneIdx = craneIdx;
                }
            }
            craneIdx++;
        }
    
        if (bestCraneIdx != -1) {
            dock->craneUsed[bestCraneIdx] = true;  // use this crane to unload cargo
            msg_to_val(4, ship->id, ship->direction, dock->id, cargoIdx, bestCraneIdx);
            ship->cargoProcessed++;
            cargoIdx++;
        } else {
            break;
        }
    }
    }


void timestep_inc() {
    MessageStruct message;
    message.mtype = 5;
    if (msgsnd(mqid, &message, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
        perror("Error sending message to validation");
        exit(EXIT_FAILURE);
    }
}
 
void crane_usage() {
     for (int i = 0; i < n; i++) {
        Dock *dock = &docks[i];
        int used = 0;
        for (int j = 0; j < dock->numCranes; j++) {
            if (dock->craneUsed[j]) {
                used++;
            }
        }
         printf("Dock %d | Cranes Used: %d/%d \n",dock->id, used, dock->numCranes);
    }
 }





char **gen_authString(int length, int *totalCount) {
     long long total = 5;  
   
     for (int i = 1; i < length - 1; i++) {
        total *= 6;
    }
   
     if(length > 1){
        total *= 5;
     }
    *totalCount = (int)total;
     char **allStrings = (char **)malloc(total * sizeof(char *));
    if (!allStrings) {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
   
    for (int i = 0; i < total; i++) {
        allStrings[i] = (char *)malloc((length + 1) * sizeof(char));
        if (!allStrings[i]) {
            perror("Memory allocation failed");
            exit(EXIT_FAILURE);
        }
    }
    char validChars[] = "56789.";
    char validEndChars[] = "56789"; 
   
    int index = 0;
   
     if (length == 1) {
         for (int i = 0; i < 5; i++) {
            allStrings[index][0] = validEndChars[i];
            allStrings[index][1] = '\0';
            index++;
        }
    }
    else{
       
        int counter = 0;
        if (total > 0) {
            do {
                int tempCounter = counter;
                allStrings[index][0] = validEndChars[tempCounter % 5];
                tempCounter /= 5;
        
                int pos = 1;
                if (length - 1 > 1) {
                    do {
                        allStrings[index][pos] = validChars[tempCounter % 6];
                        tempCounter /= 6;
                        pos++;
                    } while (pos < length - 1);
                }
        
                if (length > 1) {
                    allStrings[index][length - 1] = validEndChars[tempCounter % 5];
                }
        
                allStrings[index][length] = '\0';
                index++;
                counter++;
            } while (counter < total);
        }
            }
   
    return allStrings;
}



 void *authStringThreadFunc(void *arg) {
    Thr_data *data = (Thr_data *)arg;
    int mqid = data->solverMsgid;

     SolverRequest request;
    request.mtype = 1;
    request.dockId = data->dockId;

    if (msgsnd(mqid, &request, sizeof(request) - sizeof(long), 0) == -1) {
        perror("Error sending target dock to solver");
        return NULL;
    }

    int i = data->startIndex;
    while (i < data->endIndex) {
        pthread_mutex_lock(data->mutex);
        if (*(data->found)) {
            pthread_mutex_unlock(data->mutex);
            break;
        }
        pthread_mutex_unlock(data->mutex);

        request.mtype = 2;
        strcpy(request.authStringGuess, data->guesses[i]);

        if (msgsnd(mqid, &request, sizeof(request) - sizeof(long), 0) == -1) {
            perror("Error sending auth string guess to solver");
            i++;
            continue;
        }
        SolverResponse response;
        if(msgrcv(mqid, &response, sizeof(response) - sizeof(long), 3, 0) == -1){
            perror("Error receiving response from solver");
            i++;
            continue;
        }
        if(response.guessIsCorrect == 1){
            pthread_mutex_lock(data->mutex);
            if (!*(data->found)) {
                *(data->found) = true;
                data->correctGuess = strdup(data->guesses[i]);
            }
            pthread_mutex_unlock(data->mutex);
            break;
        }
        else if(response.guessIsCorrect == -1){
            break;
        }

        i++;
    }

    return NULL;
}

// Improved auth string guessing using multithreading
bool guess_authString(int dockId, int freqLength) {
    if (freqLength <= 0) {
        return false;  // Invalid length
    }
   
    int totalStrings;
    char **guesses = gen_authString(freqLength, &totalStrings);
    bool found = false;
    char *correctGuess = NULL;
   
    //multiple threads to guess in parallel
    pthread_t threads[m];
    Thr_data threadData[m];
    pthread_mutex_t mutex;
    pthread_mutex_init(&mutex, NULL);
   
    //divide work among threads
    int thr_str_size = totalStrings / m;
    for (int i=0; i<m; i++) {
        threadData[i].startIndex = i * thr_str_size;
        threadData[i].guesses = guesses;
        threadData[i].solverMsgid = solver_ids[i];
        threadData[i].endIndex = (i == m - 1) ? totalStrings : (i + 1) * thr_str_size;
        threadData[i].found = &found;
        threadData[i].mutex = &mutex;
        threadData[i].correctGuess = NULL;
        threadData[i].length = freqLength;
        threadData[i].dockId = dockId;
       
        pthread_create(&threads[i],NULL,authStringThreadFunc,&threadData[i]);
    }
   
    //wait for all threads to complete
    for (int i = 0; i < m; i++) {
        pthread_join(threads[i], NULL);
        if (threadData[i].correctGuess) {
            correctGuess = threadData[i].correctGuess;
        }
    }
   
    //clean up
    for (int i = 0; i < totalStrings; i++) {
        free(guesses[i]);
    }
    free(guesses);
    pthread_mutex_destroy(&mutex);
   
    //if found, copy the correct guess to shared memory
    if (correctGuess) {
        strcpy(sharedMemory->authStrings[dockId], correctGuess);
        free(correctGuess);
        return true;
    }
   
    return false;
}
 


// Count available docks and emergency ships
void cnt_available_docks(int *num_free_docks, int *emergencyShipCount) {
    *num_free_docks = 0;
    *emergencyShipCount = 0;
   
    for (int i = 0; i < n; i++) {
        if (!docks[i].isOccupied) {
            (*num_free_docks)++;
        }
    }
   
    for (int i = 0; i < nships; i++) {
        if (ships[i].status == 0 && ships[i].emergency == 1) {
            (*emergencyShipCount)++;
        }
    }
}

//process emergency ships
void process_emg_ships() {
    int emergencyShipsAssigned = 0;
   
    for (int i = 0; i < nships; i++) {
        if (ships[i].status == 0 && ships[i].emergency == 1) {
            int dockId = calc_optDock(&ships[i]);
           
            if (dockId != -1) {
                ships[i].dockId = dockId; //dock the emergency ship
                ships[i].status = 1;  //ship docked
               
                docks[dockId].isOccupied = true;
                docks[dockId].occupiedByShipId = ships[i].id;
                docks[dockId].occupiedByDirection = ships[i].direction;
                docks[dockId].dockingTimestep = curr_timestep;
                docks[dockId].cargoFullyMoved = false;
               
                // Clear crane usage
                for (int j = 0; j < docks[dockId].numCranes; j++) {
                    docks[dockId].craneUsed[j] = false;
                }
               
                // Send docking message to validation
                msg_to_val(2, ships[i].id, ships[i].direction, dockId, 0, 0);
               
                emergencyShipsAssigned++;
            }
        }
    }
}

void freq(char *gs) {
    for (int i=0; gs[i]!='\0'; i++) {
        char ch = gs[i];
        if (ch== '.') 
            ch= '*';
    }
}

// Process regular incoming ships
void process_reg_ships() {
    int i = 0;
    if (nships > 0) {
        do {
            Ship *ship = &ships[i];
    
            // Skip ships that are not regular incoming waiting ships
            if (ship->status != 0 || ship->direction != 1 || ship->emergency != 0) {
                i++;
                continue;
            }
    
            // Check if the ship is still within its waiting time
            if (!check_time(ship, curr_timestep)) {
                i++;
                continue;  // Ship will leave and return later
            }
    
            // Try to dock the ship
            int dockId = calc_optDock(ship);
            if (dockId != -1) {
                // Dock the ship
                ship->dockId = dockId;
                ship->status = 1;  // Ship is now docked
    
                docks[dockId].isOccupied = true;
                docks[dockId].occupiedByDirection = ship->direction;
                docks[dockId].occupiedByShipId = ship->id;
                docks[dockId].cargoFullyMoved = false;
                docks[dockId].dockingTimestep = curr_timestep;
    
                // Clear crane usage
                for (int j = 0; j < docks[dockId].numCranes; j++) {
                    docks[dockId].craneUsed[j] = false;
                }
    
                // Send docking message to validation
                msg_to_val(2, ship->id, ship->direction, dockId, 0, 0);
            }
    
            i++;
        } while (i < nships);
    }
    }

 void process_out_ships() {
    for (int i = 0; i < nships; i++) {
        Ship *ship = &ships[i];
       
         if (ship->status != 0 || ship->direction != -1) {
            continue;
        }
       
         int dockId = calc_optDock(ship);
        if (dockId != -1) {
             ship->dockId = dockId;
            ship->status = 1;   
           
            docks[dockId].isOccupied = true;
            docks[dockId].occupiedByShipId = ship->id;
            docks[dockId].occupiedByDirection = ship->direction;
            docks[dockId].dockingTimestep = curr_timestep;
            docks[dockId].cargoFullyMoved = false;
           
             for (int j = 0; j < docks[dockId].numCranes; j++) {
                docks[dockId].craneUsed[j] = false;
            }
           
             msg_to_val(2, ship->id, ship->direction, dockId, 0, 0);
        }
    }
}
 
void Sorting(int arr[], int n) {
     for (int i = 0; i < n - 1; i++) {
         bool flag = false;
        
         for (int j = 0; j < n - 1 - i; j++) {
             if (arr[j] > arr[j + 1]) {
                 int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
                flag = true;
            }
        }

         if (flag == false) {
            break;
        }
    }
    for (int i = 0; i < n; i++) {
        printf("%d ", arr[i]);
    }
    printf("\n");
}


void process_dock_helper(Dock *dock) {
     if (!dock->isOccupied) {
        return;
    }

    // Find the ship docked at this dock
    Ship *ship = find_ship(dock->occupiedByShipId, dock->occupiedByDirection);
    if (!ship) {
        fprintf(stderr, "error: Ship %d with direction %d not found\n",dock->occupiedByShipId, dock->occupiedByDirection);
        return;
    }

     if (dock->dockingTimestep == curr_timestep) {
        return;
    }

     if (ship->direction == 1) {   
        unload_cargo(ship, dock);
    } else {   
        load_cargo(ship, dock);
    }

     if (ship->cargoProcessed == ship->numCargo && !dock->cargoFullyMoved) {
        dock->cargoFullyMoved = true;
        dock->lastCargoMovedTimestep = curr_timestep;
    }

     if (dock->cargoFullyMoved && dock->lastCargoMovedTimestep != curr_timestep) {
        int freqLength = dock->lastCargoMovedTimestep - dock->dockingTimestep;

        if (freqLength > 0 && guess_authString(dock->id, freqLength)) {
            // Undock the ship
            msg_to_val(3, ship->id, ship->direction, dock->id, 0, 0);

             ship->status = 2;  // Serviced
            dock->isOccupied = false;
            dock->cargoFullyMoved = false;
            dock->occupiedByShipId = -1;
            dock->occupiedByDirection = 0;
        }
    }
}

void process_Docks() {
    for (int i = 0; i < n; i++) {
        process_dock_helper(&docks[i]);
    }
}

int main(int argc, char *argv[]) {
    if(argc != 2){
        fprintf(stderr, "invalid usage , format is %s <testcase_number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int tc = atoi(argv[1]);
    printf(" taken input is : %d,",tc);
    srand(time(NULL));  
   

    char inp_path[100];
    sprintf(inp_path, "testcase%d/input.txt", tc);
   
    FILE *fp = fopen(inp_path, "r");
    if(fp == NULL){
        perror("error opening input file");
        exit(EXIT_FAILURE);
    }
   //taking inputs
    key_t shm_key, main_q_key;
    fscanf(fp, "%d", &shm_key);
    fscanf(fp, "%d", &main_q_key);
   //debug :   printf(" inputs : shmkey and msg queue key : %d   %d \n",shm_key,main_q_key);
     shmid = shmget(shm_key, sizeof(MainSharedMemory), 0666);
    if (shmid == -1) {
        perror("error connecting to shared memory");
        exit(EXIT_FAILURE);
    }


    sharedMemory = (MainSharedMemory *)shmat(shmid, NULL, 0);
    if (sharedMemory == (void *)-1) {
        perror("error attaching shared memory");
        exit(EXIT_FAILURE);
    }
   
    mqid = msgget(main_q_key, 0666);
    if (mqid == -1) {
        perror("error connecting to message queue");
        exit(EXIT_FAILURE);
    }
   
    fscanf(fp, "%d", &m);
    for (int i = 0; i < m; i++) {
        key_t solver_key;
        fscanf(fp, "%d", &solver_key);

        solver_ids[i] = msgget(solver_key, 0666);
        if (solver_ids[i] == -1) {
            perror("error connecting to solver message queue");
            exit(EXIT_FAILURE);
        }
    }
   
    fscanf(fp, "%d", &n);
    for (int i = 0; i < n; i++) {
        docks[i].id = i;
        fscanf(fp, "%d", &docks[i].category);
        docks[i].numCranes = docks[i].category;
       
        for (int j = 0; j < docks[i].numCranes; j++) {
            fscanf(fp, "%d", &docks[i].craneCapacities[j]);
        }
       
        docks[i].isOccupied = false;
        docks[i].cargoFullyMoved = false;
        docks[i].occupiedByShipId = -1;
        docks[i].occupiedByDirection = 0;
    }
   
    fclose(fp);
    printf("taken input successfully! \n");
    MessageStruct m;

    bool all_ships_done = false;
   
    printf("scheduling starting... \n");
     while (!all_ships_done) {
         if (msgrcv(mqid, &m, sizeof(MessageStruct) - sizeof(long), 1, 0) == -1) {
            perror("Error in receiving messages from validation!!! ");
            exit(EXIT_FAILURE);
        }
       
         curr_timestep=m.timestep;
       //printf("debuging : current timestep %d ",curr_timestep);
        if(m.isFinished==1){
            all_ships_done = true;
            printf("done with all ships ...  exiting\n ");
            break;
        }
        // here we are handling ship requests
        //printf("Handling ship requests! ");
        //printf("debug:Calling new_ship_req function!")
        new_ship_req(m.numShipRequests);
        qsort(ships, nships, sizeof(Ship), cmp_ships);
        int num_free_docks, emergencyShipCount;
        cnt_available_docks(&num_free_docks, &emergencyShipCount);
       
        process_emg_ships();
        process_reg_ships();  
        process_out_ships();
        process_Docks();
        timestep_inc();
    }
    //shared memory cleanup
    if(shmdt(sharedMemory) == -1){
        perror("error detaching shared memory\n");
    }
    return 0;
}

package main

import (
	"net/http"

	"github.com/coreos/etcd/clientv3"
	"github.com/gin-gonic/gin"
	"github.com/infinivision/vectodb"
	"github.com/pkg/errors"
	log "github.com/sirupsen/logrus"
	"golang.org/x/net/context"
)

type ReqAcquire struct {
	DbID     int    `json:"dbID"`
	NodeAddr string `json:"nodeAddr"`
}

type RspAcquire struct {
	ReqAcquire
	Err string `json:"err"`
}

type ReqRelease struct {
	DbID int `json:"dbID"`
}
type RspRelease struct {
	ReqRelease
	Err string `json:"err"`
}

type ReqAdd struct {
	DbID int       `json:"dbID"`
	Xb   []float32 `json:"xb"`
	Xid  uint64    `json:"xid"`
}

type RspAdd struct {
	Xid uint64 `json:"xid"`
	Err string `json:"err"`
}

type ReqSearch struct {
	DbID int       `json:"dbID"`
	Xq   []float32 `json:"xq"`
}

type RspSearch struct {
	Xid      uint64  `json:"xid"`
	Distance float32 `json:"distance"`
	Err      string  `json:"err"`
}

type ControllerConf struct {
	ListenAddr string
	EtcdAddr   string
	RedisAddr  string
	Dim        int
	DisThr     float64
	SizeLimit  int

	EurekaAddr string
	EurekaApp  string
}

type Controller struct {
	conf      *ControllerConf
	rwlock	  sync.RWMutext
	dbls      map[int]*vectodb.VectoDBLite
	hc *http.Client
	etcdCli   *clientv3.Client
	isLeader  bool
	curLeader string
	ctx       context.Context
	ctxL      context.Context
	cancelL   context.CancelFunc
}

func NewControllerConf() (conf *ControllerConf) {
	return &ControllerConf{
		ListenAddr: "127.0.0.1:8080",
		EctdAddr:   "127.0.0.1:2379",
		RedisAddr:  "127.0.0.1:6379",
		Dim:        512,
		DisThr:     0.9,
		SizeLimit:  10000,
		EurekaAddr: "http://127.0.0.1:8761/eureka",
		EurekaApp:  "vectodblite-cluster",
	}
}

func NewController(conf *ControllerConf, ctx context.Context) (ctl *Controller) {
	ctl = &Controller{
		conf: conf,
		dbls: make(map[string]*vectodb.VectoDBLite),
		hc:   &http.Client{Timeout: time.Second * 5},
		ctx:  ctx,
	}
	var etcdCli *clientv3.Client
	var err error
	if etcdCli, _, err = NewEtcdClient(conf.EtcdAddr); err != nil {
		log.Fatalf("got error %+v", err)
	}
	ctl.etcdCli = etcdCli
	StartElection(ctx, etcdCli, conf.EurekaApp, conf.ListenAddr, ctl.leaderChangedCb)
	go ctl.servHoldKeepalive(ctx)
	return
}

// refers to https://github.com/swaggo/swag#api-operation
// github.com/swaggo/swag/operation.go, (*Operation).ParseParamComment, (*Operation).ParseResponseComment
// @Description Add a vector to the given vectodblite
// @Accept  json
// @Produce  json
// @Param   add		body	main.ReqAdd	true 	"ReqAdd. If xid is 0 or ^uint64(0), the cluster will generate one."
// @Success 200 {object} main.RspAdd "RspAdd"
// @Failure 301 "redirection"
// @Failure 400
// @Router /api/v1/add [post]
func (ctl *Controller) HandleAdd(c *gin.Context) {
	var reqAdd ReqAdd
	var err error
	if err = c.ShouldBind(&reqAdd); err != nil {
		err = errors.Wrap(err, "")
		log.Printf("got error %+v", err)
		c.String(http.StatusBadRequest, err.Error())
	} else {
		var rspAdd RspAdd
		var dbl *vectodb.VectoDBLite
		ctl.rwlock.RLock()
		defer ctl.rwlock.RUnlock()
		if dbl, err = ctl.getVectoDBLite(c, dbID); err != nil {
			rspAdd.Err = err.Error()
			log.Errorf("got error %+v", err)
			c.JSON(200, rspAdd)
			return
		} else if dbl == nil {
			//already return a response
			return
		}
		if reqAdd.Xid == 0 || reqAdd.Xid == ^uint64(0) {
			rspAdd.Xid, err = dbl.Add(reqAdd.DbID, reqAdd.Xb)
		} else {
			rspAdd.Xid = reqAdd.Xid
			err = dbl.AddWithId(reqAdd.DbID, reqAdd.Xb, rspAdd.Xid)
		}
		if err != nil {
			rspAdd.Err = err.Error()
			log.Errorf("got error %+v", err)
		}
		c.JSON(200, rspAdd)
	}
}

// @Description Search a vector in the given vectodblite
// @Accept  json
// @Produce  json
// @Param   search		body	main.ReqSearch	true 	"ReqSearch"
// @Success 200 {object} main.RspSearch "RspSearch"
// @Failure 301 "redirection"
// @Failure 400
// @Router /api/v1/search [post]
func (ctl *Controller) HandleSearch(c *gin.Context) {
	var reqSearch ReqSearch
	var err error
	if err = c.ShouldBind(&reqSearch); err != nil {
		err = errors.Wrap(err, "")
		log.Printf("got error %+v", err)
		c.String(http.StatusBadRequest, err.Error())
	} else {
		var rspSearch RspSearch
		var dbl *vectodb.VectoDBLite
		var dstNode string
		ctl.rwlock.RLock()
		defer ctl.rwlock.RUnlock()
		if dbl, err = ctl.getVectoDBLite(c, dbID); err != nil {
			rspSearch.Err = err.Error()
			log.Errorf("got error %+v", err)
			c.JSON(200, rspAdd)
			return
		} else if dbl == nil {
			//already return a response
			return
		}
		rspSearch.Xid, rspSearch.Distance, err = dbl.Search(reqSearch.DbID, reqSearch.Xq)
		if err != nil {
			rspSearch.Err = err.Error()
			log.Printf("got error %+v", err)
		}
		c.JSON(200, rspSearch)
	}
}

// assumes RLock is holded
func (ctl *Controller) getVectoDBLite(c *gin.Context, dbID int) (dbl *vectodb.VectoDBLite, err error) {
	if dbl, ok = ctl.dbls[dbID]; ok {
		return
	}
	var dstNodeAddr string
	if ctl.isLeader {
			if dstNodeAddr, err = acquire(dbID, ctl.conf.ListenAddr); err != nil {
				return
			}
		} else {
			curLeader := ctl.curLeader
			if curLeader == "" {
				err = errors.Errorf("Need to send acquire request to the leader. However the leader is unknown.")
				return
			}
			servURL := fmt.Sprintf("http://%s/mgmt/v1/acquire", curLeader)
			reqAcquire := ReqAcquire {
				DbID: dbID,
				NodeAddr: ctl.conf.ListenAddr,
			}
			rspAcquire := &RsqAcquire{}
			if err = PostJson(ctl.hc, servURL, reqAcquire, rspAcquire); err != nil {
				return
			}
			dstNodeAddr = rspAcquire.NodeAddr
		}

		if ctl.conf.ListenAddr != dstNodeAddr {
			dstURL := *c.Request.URL
			dstURL.Host = nodeAddr
			c.Redirect(http.StatusMovedPermanently, dstURL.String())
			return
		}
		var dblNew *vectodb.VectoDBLite
		if dblNew, err = vectodb.NewVectoDBLite(ctl.conf.RedisAddr, dbID, ctl.conf.Dim, float32(ctl.conf.DisThr), ctl.conf.SizeLimit); err != nil {
			return
		}
		ctl.rwlock.RUnlock()
		ctl.rwlock.Lock()
		defer func() {
			ctl.rwlock.Unlock()
			ctl.rwlock.Rlock()
		}
		if dbl, ok = ctl.dbls[dbID]; ok {
			return
		}
		ctl.dbls[dbID] = dblNew
		dbl = dblNew
	return
}
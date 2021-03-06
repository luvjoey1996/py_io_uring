#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>

typedef struct {
    PyObject_HEAD
    struct io_uring *ring;
    PyObject *wait_submit;
} IoUringObject;

typedef struct {
    PyObject_HEAD
    struct io_uring_sqe *sqe;
    int fd;
    int error;
    int operation;
    PyObject *allocated_buffer; // buffer create by us
    Py_buffer *user_buffer; // buffer user passed in as parameter
    PyObject *data; // any object, can be reached cqe.get_data()
    void *cqeobj; // store related cqe pointer, keep single instance refer by user.
} SqeObject;

typedef struct {
    PyObject_HEAD
    struct io_uring_cqe *cqe;
    SqeObject *sqeobj;
    bool seen;
} CqeObject;

static PyTypeObject SqeType, CqeType, IoUringType;


// IoUringObject methods definitions
static void IoUring_dealloc(IoUringObject *self)
{
    Py_XDECREF((PyObject *) self->wait_submit);
    PyMem_Free(self->ring);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject *
IoUring_new(PyTypeObject *type, PyObject *args, PyObject *kwargs){
    IoUringObject *self;
    struct io_uring *ring;
    // we cache unsubmited entries to properly set sqe data field,
    // so that we can get related sqe object when wait cqe
    PyObject *wait_submit;

    self = (IoUringObject *) (type->tp_alloc(type, 0));

    if (self) {
        ring = (struct io_uring *)PyMem_Malloc(sizeof(struct io_uring));
        if (ring != NULL) {
            self->ring = ring;
        } else {
            goto error;
        }
        wait_submit = PyList_New(0);
        if (wait_submit) {
            self->wait_submit = wait_submit;
        } else {
            goto error;
        }
    }
    return (PyObject *) self;
error:
    Py_XDECREF(self);
    return (PyObject *) NULL;
}

PyDoc_STRVAR(
        get_sqe_doc, 
        "get_sqe() -> Sqe\n\n"
        "acquire an Sqe object to describe an operation, return acquired Sqe object.");

static PyObject *
IoUring_get_sqe(IoUringObject *self)
{
    SqeObject *sqeobj;
    struct io_uring_sqe *sqe;

    sqeobj = (SqeObject *) PyObject_CallObject((PyObject *) &SqeType, NULL);
    if (sqeobj) {
        // TODO: get_sqe may got queue full error
        sqe = io_uring_get_sqe(self->ring);
        sqeobj->sqe = sqe;
        if (PyList_Append(self->wait_submit, (PyObject *) sqeobj)) {
            Py_DECREF(sqeobj);
            return NULL;
        }
    }
    return (PyObject *)sqeobj;
}

PyDoc_STRVAR(
        queue_init_doc,
        "queue_init(entries[, flag]) -> None\n\n"
        "setup an context for perfoming asynchronous IO.");

static PyObject *
IoUring_queue_init(IoUringObject *self, PyObject *args)
{
    int entries;
    unsigned flag = 0;
    int ret = 0;
    if (!PyArg_ParseTuple(args, "i|I:queue_init", &entries, &flag)) {
        return NULL;
    }
    ret = io_uring_queue_init(entries, self->ring, flag);
    if (ret < 0) {
        errno = -ret;
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        queue_exit_doc,
        "queue_exit() -> None\n\n"
        "teardown io_uring instance.");

static PyObject *
IoUring_queue_exit(IoUringObject *self)
{
    io_uring_queue_exit(self->ring);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        submit_doc,
        "submit() -> int\n\n"
        "submit operations to kernel, return number of sqes submitted.");

static PyObject *
IoUring_submit(IoUringObject *self)
{
    SqeObject *sqeobj; 
    Py_ssize_t nsubmit;
    nsubmit = PyList_Size(self->wait_submit);
    for (unsigned int i = 0; i < nsubmit; i++) {
        sqeobj = (SqeObject *) PyList_GetItem(self->wait_submit, i);
        io_uring_sqe_set_data(sqeobj->sqe, sqeobj);
        Py_INCREF(sqeobj);
    }
    int ret = io_uring_submit(self->ring);
    if (ret < 0) {
        errno = -ret;
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    Py_DECREF(self->wait_submit);
    self->wait_submit = PyList_New(0);
    Py_RETURN_NONE;
}

static PyObject *
IoUring_wait_cqe_nr_impl(IoUringObject *self, unsigned wait_nr)
{
    struct io_uring_cqe *cqes;
    struct io_uring_cqe *cqe;
    int ret;
    PyObject *rlist;
    CqeObject *cqeobj;
    SqeObject *sqeobj;

    ret = io_uring_wait_cqe_nr(self->ring, &cqes, wait_nr);

    if (ret < 0) {
        errno = -ret;
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    rlist = PyList_New(wait_nr);
    for (unsigned i = 0; i < wait_nr; i++) {
        cqe = cqes + i;
        sqeobj = (SqeObject *) cqe->user_data;
        if (sqeobj->cqeobj == NULL) {
            // we store cqe instance in sqeobj->cqeobj without incref
            // to make sure only one instance has been initialized
            // during user keep a reference pointer to cqe created last time.
            // and avoid memory leak. but if last one has been gc,
            // we create a new one.
            cqeobj =(CqeObject *) PyObject_CallObject((PyObject *) &CqeType, NULL);
            cqeobj->cqe = cqe;
            Py_INCREF(sqeobj);
            cqeobj->sqeobj = sqeobj;
            sqeobj->cqeobj = cqeobj;
        } else {
            cqeobj = (CqeObject *) sqeobj->cqeobj;
            Py_INCREF(cqeobj);
        }
        if (PyList_SetItem(rlist, i, (PyObject *) cqeobj)) {
            Py_DECREF(cqeobj);
            goto error;
        }
    }
    return rlist;
error:
    Py_DECREF(rlist);
    return NULL;
}

PyDoc_STRVAR(
        wait_cqe_nr_doc,
        "wait_cqe_nr(wait_nr) -> List[Cqe]\n\n"
        "waiting for wait_nr completions, return a list of completed Cqe Object.");

static PyObject *
IoUring_wait_cqe_nr(IoUringObject *self, PyObject *args)
{
    unsigned wait_nr = 1;

    if (!PyArg_ParseTuple(args, "I:wait_cqe_nr", &wait_nr)) {
        return NULL;
    }
    return IoUring_wait_cqe_nr_impl(self, wait_nr);
}

static PyObject *
IoUring_wait_cqes(IoUringObject *self, PyObject *args)
{
    struct io_uring *ring = self->ring;
    struct io_uring_cqe *cqes;
    struct io_uring_cqe *cqe;
    unsigned wait_nr = 0;
    double timeout = 0;
    int ret;
    PyObject *rlist;
    CqeObject *cqeobj;
    SqeObject *sqeobj;

    if (!PyArg_ParseTuple(args, "I|d", &wait_nr, &timeout)) {
        return NULL;
    }
    if (timeout) {
        printf("in timeout\n");
        struct __kernel_timespec ts;
        ts.tv_sec = (long long) timeout;
        ts.tv_nsec = (long long) ((timeout - ts.tv_sec) * 1e9);
        ret = io_uring_wait_cqes(ring, &cqes, wait_nr, &ts, 0);
    } else {
        printf("out timeout\n");
        ret = io_uring_wait_cqes(ring, &cqes, wait_nr, NULL, 0);
    }

    if (ret) {
        errno = -ret;
        return PyErr_SetFromErrno(PyExc_OSError);
    }

    rlist = PyList_New(wait_nr);
    if (!rlist) {
        printf("going to error init list\n");
        goto error;
    }
    for (unsigned i = 0; i < wait_nr; i++) {
        cqe = cqes + i;
        printf("0x%p\n", cqe);
        if (cqe) {
        sqeobj = (SqeObject *) cqe->user_data;
        if (sqeobj->cqeobj == NULL) {
            // we store cqe instance in sqeobj->cqeobj without incref
            // to make sure only one instance has been initialized
            // during user keep a reference pointer to cqe created last time.
            // and avoid memory leak. but if last one has been gc,
            // we create a new one.
            cqeobj =(CqeObject *) PyObject_CallObject((PyObject *) &CqeType, NULL);
            cqeobj->cqe = cqe;
            Py_INCREF(sqeobj);
            cqeobj->sqeobj = sqeobj;
            sqeobj->cqeobj = cqeobj;
        } else {
            cqeobj = (CqeObject *) sqeobj->cqeobj;
            Py_INCREF(cqeobj);
        }
        if (PyList_SetItem(rlist, i, (PyObject *) cqeobj)) {
            Py_DECREF(cqeobj);
            printf("going to error in list set item\n");
            goto error;
        }
        }
    }
    return rlist;
error:
    Py_DECREF(rlist);
    return NULL;
}

static inline PyObject *
IoUring_wait_single_cqe(IoUringObject *self, unsigned wait_nr)
{
    struct io_uring_cqe *cqe;
    CqeObject *cqeobj;
    SqeObject *sqeobj;
    int ret;
    ret = io_uring_wait_cqe_nr(self->ring, &cqe, wait_nr);
    if (ret < 0) {
        errno = -ret;
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    sqeobj = (SqeObject *) cqe->user_data;
    if (sqeobj->cqeobj == NULL) {
        cqeobj =(CqeObject *) PyObject_CallObject((PyObject *) &CqeType, NULL);
        if (cqeobj == NULL) {
            return NULL;
        }
        cqeobj->cqe = cqe;
        Py_INCREF(sqeobj);
        cqeobj->sqeobj = sqeobj;
        sqeobj->cqeobj = cqeobj;
    } else {
        cqeobj = (CqeObject *) sqeobj->cqeobj;
        Py_INCREF(cqeobj);
    }
    if (cqeobj == NULL) {
        return NULL;
    }
    cqeobj->cqe = cqe;
    cqeobj->sqeobj = (SqeObject *) cqe->user_data;
    return (PyObject *) cqeobj;
}

PyDoc_STRVAR(
        wait_cqe_doc,
        "wait_cqe() -> Cqe\n\n"
        "waiting for a completion, return the completed Cqe."
        );

static PyObject *
IoUring_wait_cqe(IoUringObject *self)
{
    return IoUring_wait_single_cqe(self, 1);
}

PyDoc_STRVAR(
        peek_cqe_doc,
        "peek_cqe() -> Cqe\n\n"
        "peek for a completion without waiting, return the completed Cqe or None.");

static PyObject *
IoUring_peek_cqe(IoUringObject *self)
{
    return IoUring_wait_single_cqe(self, 0);
}

PyDoc_STRVAR(
        cqe_seen_doc,
        "cqe_seen(cqe) -> None\n\n"
        "mark this Cqe Object as processed. must be called.");

static PyObject *
IoUring_cqe_seen(IoUringObject *self, PyObject *args)
{
    CqeObject *cqe;
    if (!PyArg_ParseTuple(args, "O:cqe_seen", &cqe)) {
        return NULL;
    }
    if (!cqe->seen) {
        // because we incref sqeobj when we are doing io_uring submit
        // so we should decref when related cqe has been processed
        // after cqe_seen this cqe would never be created by wait_cqe
        io_uring_cqe_seen(self->ring, cqe->cqe);
        cqe->seen = true;
        Py_DECREF(cqe->sqeobj);
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        sq_ready_doc,
        "sq_ready() -> int\n\n"
        "return number of ready sqe in submition queue."
        );

static PyObject *
IoUring_sq_ready(IoUringObject *self)
{
    unsigned nready;
    nready = io_uring_sq_ready(self->ring);
    return PyLong_FromLong(nready);
}

PyDoc_STRVAR(
        sq_space_left_doc,
        "sq_space_left() -> int\n\n"
        "return number of sqe can be acquired in submition queue.");

static PyObject *
IoUring_sq_space_left(IoUringObject *self)
{
    unsigned n;
    n = io_uring_sq_space_left(self->ring);
    return PyLong_FromLong(n);
}

PyDoc_STRVAR(
        cq_ready_doc,
        "cq_ready() -> int\n\n"
        "return the number of completed cqe in completion queue.");

static PyObject *
IoUring_cq_ready(IoUringObject *self)
{
    unsigned nready = io_uring_cq_ready(self->ring);
    return PyLong_FromLong(nready);
}

static PyObject *
IoUring_cq_event_fd_enabled(IoUringObject *self)
{
    bool enabled = io_uring_cq_eventfd_enabled(self->ring);
    return PyBool_FromLong(enabled);
}

// SqeObject methods definitions

static PyObject *
Sqe_new(PyTypeObject *type, PyObject *args, PyObject *kwls)
{
    SqeObject *self;
    self = (SqeObject *) (type->tp_alloc(type, 0));
    if (self != NULL) {
        self->fd = -1;
        self->error = 0;
        self->operation = -1;
        self->allocated_buffer = NULL;
        Py_buffer *buf = (Py_buffer *) PyMem_Malloc(sizeof(Py_buffer));
        if (buf != NULL) {
            self->user_buffer = buf;
        } else {
            Py_DECREF(self);
            return NULL;
        }
        self->user_buffer->obj = NULL;
        self->cqeobj = NULL;
    } else {
        return NULL;
    }
    Py_INCREF(Py_None);
    self->data = Py_None;
    return (PyObject*) self;
}

static void Sqe_reinit_buffer(SqeObject *self)
{
    if (self->user_buffer->obj != NULL) {
        PyBuffer_Release(self->user_buffer);
        self->user_buffer->obj = NULL;
    }
    if (self->allocated_buffer != NULL) {
        Py_DECREF(self->allocated_buffer);
        self->allocated_buffer = NULL;
    }
}

static void Sqe_dealloc(SqeObject *self)
{
    Sqe_reinit_buffer(self);
    PyMem_Free(self->user_buffer);
    Py_DECREF(self->data);
    Py_TYPE(self)->tp_free((PyObject *) self);
    return;
}

PyDoc_STRVAR(
        prep_send_doc,
        "prep_send(fd, buf[, flags]) -> None\n\n"
        "Issue the equivalent of a send(2) system call.");

static PyObject *
Sqe_prep_send(SqeObject *self, PyObject *args)
{
    char *buf;
    int fd, len, flags = 0;

    Sqe_reinit_buffer(self);
    if (!PyArg_ParseTuple(args, "iy*|i:prep_send", &fd, self->user_buffer, &flags)) {
        return NULL;
    }
    buf = self->user_buffer->buf;
    len = self->user_buffer->len;
    io_uring_prep_send(self->sqe, fd, buf, len, flags);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_recv_doc,
        "prep_recv(fd, len[, flags]) -> None\n\n"
        "Issue the equivalent of recv(2) system call.");

static PyObject *
Sqe_prep_recv(SqeObject *self, PyObject *args)
{
    int fd, len, flags = 0;

    Sqe_reinit_buffer(self);
    if (!PyArg_ParseTuple(args, "ii|i:prep_recv", &fd, &len, &flags)) {
        return NULL;
    }
    self->allocated_buffer = PyBytes_FromStringAndSize(NULL, len);
    char *buf = PyBytes_AS_STRING(self->allocated_buffer);
    io_uring_prep_recv(self->sqe, fd, buf, len, flags);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_connect_doc,
        "prep_connect(fd, addr) -> None\n\n"
        "Issue the equivalent of a connect(2) system call.");

static PyObject *
Sqe_prep_connect(SqeObject *self, PyObject *args)
{
    // TODO: support ipv6 connect
    PyObject *addrobj;
    struct sockaddr_in *addr;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    char *ip;
    unsigned port;
    int fd;

    Sqe_reinit_buffer(self);
    if (!PyArg_ParseTuple(args, "iO!:prep_connect", &fd, &PyTuple_Type, &addrobj)) {
        return NULL;
    }
    if (!PyArg_ParseTuple(addrobj, "sI:prep_connect", &ip, &port)) {
        return NULL;
    }

    self->allocated_buffer = PyBytes_FromStringAndSize((char *) 0, sizeof(struct sockaddr_in));
    addr = (struct sockaddr_in*) PyBytes_AS_STRING(self->allocated_buffer);
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, ip, &(addr->sin_addr));
    addr->sin_port = htons(port);
    io_uring_prep_connect(self->sqe, fd, (struct sockaddr *) addr, addrlen);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_accept_doc,
        "prep_accept(fd[, flags]) -> None\n\n"
        "Issue the equivalent of an accept4(2) system call.");

static PyObject *
Sqe_prep_accept(SqeObject *self, PyObject *args)
{
    // TODO: support ipv6 accept
    struct sockaddr_in *addr;
    socklen_t *addrlen;
    int fd, flags = 0;

    Sqe_reinit_buffer(self);
    if (!PyArg_ParseTuple(args, "i|i:prep_accept", &fd, &flags)) {
        return NULL;
    }
    self->allocated_buffer = PyBytes_FromStringAndSize(NULL, sizeof(struct sockaddr_in) + sizeof(socklen_t));
    addr = (struct sockaddr_in*) PyBytes_AS_STRING(self->allocated_buffer);
    addrlen = (socklen_t *)(addr + 1);
    *addrlen = sizeof(struct sockaddr_in);
    io_uring_prep_accept(self->sqe, fd, (struct sockaddr *) addr, addrlen, flags);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

static PyObject *
Sqe_convert_address(SqeObject *self)
{
    struct sockaddr_in *addr;
    socklen_t *addrlen;
    unsigned short port;
    addr = (struct sockaddr_in*) PyBytes_AS_STRING(self->allocated_buffer);
    addrlen = (socklen_t *)(addr + 1);
    char *ip = PyMem_Malloc(*addrlen);
    if (inet_ntop(AF_INET, &addr->sin_addr, ip, 1024) == NULL) {
        PyMem_Free(ip);
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    port = ntohs(addr->sin_port);
    PyObject *ret = Py_BuildValue("sh", ip, port);
    PyMem_Free(ip);
    return ret;
}

PyDoc_STRVAR(
        prep_read_doc,
        "prep_read(fd, len[, flags]) -> None\n\n"
        "Issue the equivalent of a read(2) system call.");

static PyObject *
Sqe_prep_read(SqeObject *self, PyObject *args)
{
    int fd, len, offset;
    Sqe_reinit_buffer(self);

    if (!PyArg_ParseTuple(args, "ii|i:prep_read", &fd, &len, &offset)) {
        return NULL;
    }
    self->allocated_buffer = PyBytes_FromStringAndSize(NULL, len);
    char *buf = PyBytes_AS_STRING(self->allocated_buffer);
    io_uring_prep_read(self->sqe, fd, buf, len, offset);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_write_doc,
        "prep_write(fd, buf[, flags]) -> None\n\n"
        "Issue the equivalent of a write(2) system call.");

static PyObject *
Sqe_prep_write(SqeObject *self, PyObject *args)
{
    char *buf;
    int fd, len, offset;

    Sqe_reinit_buffer(self);
    if (!PyArg_ParseTuple(args, "iy*|i:prep_write", &fd, self->user_buffer, &offset)) {
        return NULL;
    }
    buf = self->user_buffer->buf;
    len = self->user_buffer->len;
    io_uring_prep_write(self->sqe, fd, buf, len, offset);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_nop_doc,
        "prep_nop() -> None\n\n"
        );

static PyObject *
Sqe_prep_nop(SqeObject *self)
{
    io_uring_prep_nop(self->sqe);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_timeout_doc,
        "prep_timeout(timeout[, flags]) -> None\n\n"
        "prepare a timeout operation");

static PyObject *
Sqe_prep_timeout(SqeObject *self, PyObject *args)
{
    double timeout;
    unsigned int count= 0, flags = 0;

    Sqe_reinit_buffer(self);
    if (!PyArg_ParseTuple(args, "d|II:prep_timeout", &timeout, &count, &flags)) {
        return NULL;
    }
    self->allocated_buffer = PyBytes_FromStringAndSize(NULL, sizeof(struct __kernel_timespec));
    struct __kernel_timespec *ts = (struct __kernel_timespec *) PyBytes_AS_STRING(self->allocated_buffer);
    ts->tv_sec = (long long) timeout;
    ts->tv_nsec = (long long) ((timeout - ts->tv_sec) * 1e9);
    io_uring_prep_timeout(self->sqe, ts, count, flags);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_timeout_remove_doc,
        "prep_timeout_remove(sqe[, flags]) -> None\n\n"
        "prepare an attempt to remove an existing timeout operation.");

static PyObject *
Sqe_prep_timeout_remove(SqeObject *self, PyObject *args)
{
    // TODO: impl
    SqeObject *timeout;
    unsigned flags = 0;

    if (!PyArg_ParseTuple(args, "O!I|prep_timeout_remove", &timeout, &flags)) {
        return NULL;
    }
    io_uring_prep_timeout_remove(self->sqe, (__u64) timeout, flags);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_cancel_doc,
        "prep_cancel(sqe) -> None\n\n"
        "prep an operation to cancel submitted operation.");

static PyObject *
Sqe_prep_cancel(SqeObject *self, PyObject *args)
{
    // TODO: impl
    SqeObject *cancel;
    unsigned flags = 0;

    if (!PyArg_ParseTuple(args, "O!I:prep_cancel", &SqeType, &cancel, &flags)) {
        return NULL;
    }
    io_uring_prep_cancel(self->sqe, cancel, flags);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_close_doc,
        "prep_close(fd) -> None\n\n"
        "prepare an operation to close fd.");

static PyObject *
Sqe_prep_close(SqeObject *self, PyObject *args)
{
    int fd;
    if (!PyArg_ParseTuple(args, "i:prep_close", &fd)) {
        return NULL;
    }
    io_uring_prep_close(self->sqe, fd);
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        prep_openat_doc,
        "prep_openat() -> None\n\n"
        "Issue the equivalent of a openat(2) system call");

static PyObject *
Sqe_prep_openat(SqeObject *self, PyObject *args)
{
    self->operation = self->sqe->opcode;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(
        set_data_doc,
        "set_data(data) -> None\n\n"
        "set the data related to this sqe, can be reached by related cqe.");

static PyObject *
Sqe_set_data(SqeObject *self, PyObject *args)
{
    PyObject *data;
    if (!PyArg_ParseTuple(args, "O:set_data", &data)) {
        return NULL;
    }
    Py_INCREF(data);
    Py_DECREF(self->data);
    self->data = data;
    Py_RETURN_NONE;
}

// CqeObject methods definitions
static PyObject *
Cqe_new(PyTypeObject *type, PyObject *args, PyObject *kwlist)
{
    CqeObject *self;
    self = (CqeObject *) (type->tp_alloc(type, 0));
    if (self != NULL) {
        self->seen = false;
    } else {
        return NULL;
    }
    return (PyObject*) self;
}

static void
Cqe_dealloc(CqeObject *self)
{
    // when user keep an reference to related sqeobj
    // the specified sqeobj won't be gc, and also this
    // cqe may not call cqe_seen method, so we reset sqeobj's
    // cqeobj field, to allow new one can be created by
    // wait cqe or peek cqe method without segmentfault
    // caused by sqeobj's invalid cqeobj pointer
    self->sqeobj->cqeobj = NULL;
    Py_XDECREF((PyObject *) self->sqeobj);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

PyDoc_STRVAR(
        get_data_doc,
        "get_data() -> any\n\n"
        "get data set in related sqe.");

static PyObject *
Cqe_get_data(CqeObject *self, PyObject *args)
{
    PyObject *data = (PyObject *) self->sqeobj->data;
    Py_INCREF(data);
    return data;
}

PyDoc_STRVAR(
        res_doc,
        "res() -> int\n\n"
        "operation res.");

static PyObject *
Cqe_res(CqeObject *self, PyObject *args)
{
    return PyLong_FromLong(self->cqe->res);
}

static PyObject *
Cqe_getresult(CqeObject *self)
{
    SqeObject *sqeobj = self->sqeobj;
    // struct io_uring_cqe *cqe = self->cqe;
    int operation = sqeobj->operation;
    int res = self->cqe->res;
    if (res < 0) {
        errno = -res;
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    switch (operation) {
        case IORING_OP_NOP:
            Py_RETURN_NONE;
        case IORING_OP_READ:
        case IORING_OP_RECV:
            if (res != PyBytes_GET_SIZE(sqeobj->allocated_buffer)
                    && _PyBytes_Resize(&(sqeobj->allocated_buffer), res)) {
                return NULL;
            }
            Py_INCREF(sqeobj->allocated_buffer);
            return sqeobj->allocated_buffer;
        default:
            return PyLong_FromLong(res);
    }
}

// IoUringType definition

static PyMethodDef IoUring_methods[] = {
    {"get_sqe", (PyCFunction) IoUring_get_sqe, METH_NOARGS, get_sqe_doc},
    {"queue_init", (PyCFunction) IoUring_queue_init, METH_VARARGS, queue_init_doc},
    {"queue_exit", (PyCFunction) IoUring_queue_exit, METH_NOARGS, queue_exit_doc},
    {"submit", (PyCFunction) IoUring_submit, METH_NOARGS, submit_doc},
    {"wait_cqe_nr", (PyCFunction) IoUring_wait_cqe_nr, METH_VARARGS, wait_cqe_nr_doc},
    {"wait_cqes", (PyCFunction) IoUring_wait_cqes, METH_VARARGS, wait_cqe_nr_doc},
    {"wait_cqe", (PyCFunction) IoUring_wait_cqe, METH_NOARGS, wait_cqe_doc},
    {"peek_cqe", (PyCFunction) IoUring_peek_cqe, METH_NOARGS, peek_cqe_doc},
    {"cqe_seen", (PyCFunction) IoUring_cqe_seen, METH_VARARGS, cqe_seen_doc},
    {"sq_ready", (PyCFunction) IoUring_sq_ready, METH_NOARGS, sq_ready_doc},
    {"sq_space_left", (PyCFunction) IoUring_sq_space_left, METH_NOARGS, sq_space_left_doc},
    {"cq_ready", (PyCFunction) IoUring_cq_ready, METH_NOARGS, cq_ready_doc},
    {"cq_event_fd_enabled", (PyCFunction) IoUring_cq_event_fd_enabled, METH_NOARGS, ""},
    {NULL}
};

static PyTypeObject IoUringType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "py_io_uring.IoUring",
    .tp_doc = "IoUring Object",
    .tp_basicsize = sizeof(IoUringObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = IoUring_new,
    .tp_dealloc = (destructor) IoUring_dealloc,
    .tp_methods = IoUring_methods,
};

// SqeType definition

static PyMethodDef Sqe_methods[] = {
    {"prep_recv", (PyCFunction) Sqe_prep_recv, METH_VARARGS, prep_recv_doc},
    {"prep_send", (PyCFunction) Sqe_prep_send, METH_VARARGS, prep_send_doc},
    {"prep_connect", (PyCFunction) Sqe_prep_connect, METH_VARARGS, prep_connect_doc},
    {"prep_accept", (PyCFunction) Sqe_prep_accept, METH_VARARGS, prep_accept_doc},
    {"prep_read", (PyCFunction) Sqe_prep_read, METH_VARARGS, prep_read_doc},
    {"prep_write", (PyCFunction) Sqe_prep_write, METH_VARARGS, prep_write_doc},
    {"set_data", (PyCFunction) Sqe_set_data, METH_VARARGS, set_data_doc},
    {"convert_address", (PyCFunction) Sqe_convert_address, METH_NOARGS, ""},
    {"prep_nop", (PyCFunction) Sqe_prep_nop, METH_NOARGS, prep_nop_doc},
    {"prep_timeout", (PyCFunction) Sqe_prep_timeout, METH_VARARGS, prep_timeout_doc},
    {"prep_timeout_remove", (PyCFunction) Sqe_prep_timeout_remove, METH_VARARGS, prep_timeout_remove_doc},
    {"prep_close", (PyCFunction) Sqe_prep_close, METH_VARARGS, prep_close_doc},
    {"prep_openat", (PyCFunction) Sqe_prep_openat, METH_VARARGS, prep_openat_doc},
    {"prep_cancel", (PyCFunction) Sqe_prep_cancel, METH_VARARGS, prep_cancel_doc},
    {NULL}
};

static PyTypeObject SqeType  = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "py_io_uring.Sqe",
    .tp_doc = "Sqe Object",
    .tp_basicsize = sizeof(SqeObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = Sqe_new,
    .tp_dealloc = (destructor) Sqe_dealloc,
    .tp_methods = Sqe_methods
};

// CqeType definition

static PyMethodDef Cqe_methods[] = {
    {"res", (PyCFunction) Cqe_res, METH_NOARGS, res_doc},
    {"get_data", (PyCFunction) Cqe_get_data, METH_NOARGS, get_data_doc},
    {"getresult", (PyCFunction) Cqe_getresult, METH_NOARGS, ""},
    {NULL}
};

static PyTypeObject CqeType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "py_io_uring.Cqe",
    .tp_doc = "Cqe Object",
    .tp_basicsize = sizeof(CqeObject),
    .tp_itemsize= 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = Cqe_new,
    .tp_dealloc = (destructor) Cqe_dealloc,
    .tp_methods = Cqe_methods
};

static PyModuleDef PyIoUringModule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "py_io_uring",
    .m_doc = "Python wrapper for linux async interface io_uring",
    .m_size = -1
};


PyMODINIT_FUNC
PyInit_py_io_uring(void)
{
    PyObject *m;
    if (PyType_Ready(&IoUringType) < 0) {
        return NULL;
    }
    if (PyType_Ready(&SqeType) < 0) {
        return NULL;
    }
    if (PyType_Ready(&CqeType) < 0) {
        return NULL;
    }
    m = PyModule_Create(&PyIoUringModule);
    if (m == NULL) {
        return NULL;
    }
    Py_INCREF(&IoUringType);
    Py_INCREF(&SqeType);
    Py_INCREF(&CqeType);
    if (
            PyModule_AddObject(m, "IoUring", (PyObject *) &IoUringType) < 0 ||
            PyModule_AddObject(m, "Sqe", (PyObject *) &SqeType) < 0 ||
            PyModule_AddObject(m, "Cqe", (PyObject *) &CqeType) < 0
    )
    {
        goto error;
    }
    return m;
error:
    Py_DECREF(&IoUringType);
    Py_DECREF(&SqeType);
    Py_DECREF(&CqeType);
    Py_DECREF(m);
    return NULL;
}

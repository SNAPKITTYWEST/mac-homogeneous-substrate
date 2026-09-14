;;;; ============================================================
;;;; EXPERIMENTAL MACINTOSH LISP OBJECT MACHINE
;;;; Homogeneous substrate: cons cells = memory objects
;;;; ============================================================

(in-package :mac-lisp-experimental)

;;; --- Core object representation (cons-based) ---
(defstruct (cell (:constructor make-cell (car cdr))
                 (:print-function print-cell))
  car cdr
  (mark 0)
  (type :cons))

(defun print-cell (c stream depth)
  (declare (ignore depth))
  (format stream "#<CELL ~A . ~A>" (cell-car c) (cell-cdr c)))

;;; Memory plane simulation
(defvar *memory* (make-array 4096 :initial-element nil))
(defvar *free-list* 0)
(defvar *heap-ptr* 0)

(defun alloc-cell (a d)
  (let ((idx *heap-ptr*))
    (incf *heap-ptr*)
    (setf (aref *memory* idx) (make-cell a d))
    idx))

(defun mem-ref (addr) (aref *memory* addr))
(defun mem-set (addr val) (setf (aref *memory* addr) val))

;;; --- Classic Lisp forms that reduce to cells ---
(defun lisp-+ (x y) (+ x y))
(defun lisp-* (x y) (* x y))
(defun lisp-fma (a b c) (+ (* a b) c))

;;; The expression (+ (* A B) C) as object graph
(defvar *A* 10.0)
(defvar *B* 20.0)
(defvar *C* 5.0)

(defun make-expression ()
  ;; (fma A B C) --> cons structure
  (let* ((mul-node (alloc-cell '* (alloc-cell *A* (alloc-cell *B* nil))))
         (add-node (alloc-cell '+ (alloc-cell mul-node (alloc-cell *C* nil)))))
    add-node))

(defun eval-object (obj)
  (cond
    ((numberp obj) obj)
    ((symbolp obj) (symbol-value obj))
    ((cell-p (mem-ref obj))
     (let ((c (mem-ref obj)))
       (case (cell-car c)
         (+ (lisp-+ (eval-object (cell-car (mem-ref (cell-cdr c))))
                    (eval-object (cell-car (mem-ref (cell-cdr (mem-ref (cell-cdr c))))))))
         (* (lisp-* (eval-object (cell-car (mem-ref (cell-cdr c))))
                    (eval-object (cell-car (mem-ref (cell-cdr (mem-ref (cell-cdr c))))))))
         (fma (lisp-fma (eval-object (cell-car (mem-ref (cell-cdr c))))
                        (eval-object (cell-car (mem-ref (cell-cdr (mem-ref (cell-cdr c))))))
                        (eval-object (cell-car (mem-ref (cell-cdr (mem-ref (cell-cdr (mem-ref (cell-cdr c))))))))))
         (t (error "unknown operator")))))))

;;; --- Closures and environments as objects ---
(defstruct environment
  bindings
  parent)

(defun make-env (&optional parent)
  (make-environment :bindings (make-hash-table) :parent parent))

(defun env-lookup (env sym)
  (or (gethash sym (environment-bindings env))
      (and (environment-parent env)
           (env-lookup (environment-parent env) sym))))

(defun env-bind (env sym val)
  (setf (gethash sym (environment-bindings env)) val))

(defstruct closure
  params body env)

(defun make-closure (params body env)
  (make-closure :params params :body body :env env))

(defun apply-closure (cl args)
  (let ((new-env (make-env (closure-env cl))))
    (mapc (lambda (p a) (env-bind new-env p a))
          (closure-params cl) args)
    (eval-in-env (closure-body cl) new-env)))

;;; --- Lambda example ---
(defun make-inc-closure ()
  (make-closure '(x) '(+ x 1) (make-env)))

;;; --- Generic-ish dispatch simulation (pre-Dylan) ---
(defvar *methods* (make-hash-table :test #'equal))

(defun define-method (name specializers fn)
  (setf (gethash (cons name specializers) *methods*) fn))

(defun call-generic (name &rest args)
  (let* ((types (mapcar #'type-of args))
         (key (cons name types))
         (method (gethash key *methods*)))
    (if method
        (apply method args)
        (error "no method for ~A on ~A" name types))))

(define-method 'add '(float float) #'+)
(define-method 'add '(integer integer) #'+)
(define-method 'mul '(float float) #'*)

;;; --- Persistent image simulation ---
(defvar *image* (make-hash-table :test #'eq))

(defun intern-object (name obj)
  (setf (gethash name *image*) obj)
  obj)

(defun image-ref (name) (gethash name *image*))

;;; --- FLOP bridge back to raw memory ---
(defun lisp-to-flop (expr)
  (let ((result (eval-object expr)))
    (format t "LISP -> FLOP result: ~A~%" result)
    result))

;;; --- Cross-layer entry points ---
(defun lisp-eval (form)
  (cond
    ((numberp form) form)
    ((symbolp form) (symbol-value form))
    ((consp form)
     (case (car form)
       (+ (apply #'+ (mapcar #'lisp-eval (cdr form))))
       (* (apply #'* (mapcar #'lisp-eval (cdr form))))
       (fma (lisp-fma (lisp-eval (second form))
                      (lisp-eval (third form))
                      (lisp-eval (fourth form))))
       (lambda (make-closure (second form) (third form) (make-env)))
       (t (apply (symbol-function (car form))
                 (mapcar #'lisp-eval (cdr form))))))
    (t form)))

;;; Driver
(defun run-lisp-stack ()
  (format t "=== LISP OBJECT MACHINE READY ===~%")
  (let ((expr (make-expression)))
    (format t "Object graph root: ~A~%" expr)
    (format t "Evaluated: ~A~%" (eval-object expr)))
  (let ((inc (make-inc-closure)))
    (format t "Closure result: ~A~%" (apply-closure inc '(41))))
  (format t "Generic add: ~A~%" (call-generic 'add 3.0 4.0))
  (format t "Image objects: ~A~%" (hash-table-count *image*)))

(run-lisp-stack)

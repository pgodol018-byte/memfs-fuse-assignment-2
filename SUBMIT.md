# Submission

## Members
### Member 1
- Name: ΣΠΥΡΙΔΩΝ ΚΑΝΙΔΗΣ
- Academic ID:IIS23200

### Member 2
- Name: ΓΚΟΝΤΟΛΙΑΣ ΠΑΝΑΓΙΩΤΗΣ
- Academic ID:IIS25110

## Σύντομη περιγραφή υλοποίησης
--ΚΑΘΟΛΙΚΟ MUTEX ΚΑΙ WRAPPER ΣΥΝΑΡΤΗΣΕΙΣ
ΠΡΟΣΤΑΤΕΥΟΥΜΕ ΤΗ ΚΟΙΝΩΧΡΗΣΤΗ ΔΟΜΗ ΤΟΥ ΣΥΣΤΗΜΑΤΟΣ (the_fs) με ενα εννιαιο pthread_mutex_t. ΓΙΑ ΤΗΝ ΑΠΟΦΥΓΗ DEADLOCKS ΔΗΜΙΟΥΡΓΗΣΑΜΕ ΕΣΩΤΕΡΙΚΕΣ ΕΚΔΟΣΕΙΣ _LOCKED ΧΩΡΙΣ ΕΣΩΤΕΡΙΚΑ ΚΛΕΙΔΩΜΑΤΑ
--ΝΗΜΑ ΠΑΡΑΣΚΗΝΙΟΥ ΣΤΑΤΙΣΤΙΚΩΝ
ΔΗΜΙΟΥΡΓΗΘΗΚΕ ΕΝΑ ΑΠΟΚΟΜΜΕΝΟ prthread_deatach στην main ,το οποιο καλει ανα 5 δεύτερα τη collect_statistics(the_fs.root) προστατεύοντας τη διάσχιση του δέντρου 

--ΚΛΕΙΔΑΡΙΑ ΑΝΑΓΝΩΣΤΩΝ-ΓΡΑΦΕΩΝ
ΑΝΤΙΚΑΤΑΣΤΑΘΗΚΕ ΤΟ ΑΠΛΟ MUTEX ΜΕ READER-WRITER LOCK
--FINE-GRAINED ΚΛΕΙΔΩΜΑ ΑΝΑ ΚΟΜΒΟ
ΣΤΟ ΤΕΛΙΚΟ ΣΤΑΔΙΟ ΠΡΟΣΤΕΘΗΚΕ ΞΕΧΩΡΙΣΤΟ rwmutex σε καθε struct node


## ΧΡΟΝΟΙ ΕΚΤΕΛΕΣΗΣ 
(./test-single.sh)|real  5.983s  |user 0.558s   |system  0.331s |
(./test-mmultiple.sh)|real 5.018s  |user 0.019s |system 0.010s  |